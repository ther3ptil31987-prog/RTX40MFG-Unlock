#include "hudless_probe.h"
#include "hudless_detection.h"
#include "hudless_visualizer.h"

#include <Windows.h>
#include <d3d12.h>
#include <wrl/client.h>

#include <array>
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cwchar>
#include <mutex>
#include <vector>

namespace hudless_probe
{
namespace
{
using Microsoft::WRL::ComPtr;
namespace hd = hudless_detection;

constexpr uint32_t kTypeSlots = 96;
constexpr uint32_t kTypeDepth = 0;
constexpr uint32_t kTypeMotionVectors = 1;
constexpr uint32_t kTypeHudless = 2;
constexpr uint32_t kTypeUiColorAndAlpha = 23;
constexpr uint32_t kTypeBackbuffer = 53;
constexpr uint32_t kTypeUiAlpha = 69;

constexpr uint64_t kRecentMs = 2000;
constexpr uint64_t kNoHudlessDecisionMs = 10000;
constexpr uint64_t kFastCadenceMs = 500;
constexpr uint64_t kSlowCadenceMs = 5000;
constexpr uint32_t kFastProbes = 8;
constexpr uint64_t kStageTimeoutMs = 3000;
constexpr uint64_t kUiDescribeMs = 1000;
// A tagging pattern must persist this long before it withdraws a verified pair,
// so startup ordering or a loading screen cannot.
constexpr uint64_t kSourceSettledMs = 10000;
uint64_t gSourceSettledMs = kSourceSettledMs; // Adjustable in test builds only.
constexpr uint32_t kFinalCaptures = 2;
constexpr UINT kUnknownState = UINT_MAX;

enum class Api : uint32_t { eUnknown, eD3D12, eVulkan };
enum class Stage : uint32_t { eIdle, eArmed, eHudlessRecorded, eHudlessDeferred, eCapturing, eSubmitted };

struct Footprint
{
    uint64_t offset = 0;
    uint32_t rowPitch = 0;
};

struct TileBuffer
{
    ComPtr<ID3D12Resource> buffer;
    uint64_t bytes = 0;
    uint32_t format = 0;
    uint32_t bytesPerPixel = 0;
    uint64_t markerOffset = 0;
    std::array<Footprint, hd::kTileCount> tiles{};
};

struct Region
{
    uint32_t left = 0;
    uint32_t top = 0;
    uint32_t width = 0;
    uint32_t height = 0;
};

struct State
{
    std::mutex mutex;

    // Last described HUDless tag.
    uint32_t hudlessResourceFormat = 0;
    uint32_t hudlessFormat = 0;
    uint32_t hudlessWidth = 0;
    uint32_t hudlessHeight = 0;
    uint32_t hudlessLifecycle = 0;
    uint32_t hudlessState = 0;
    Region hudlessExtent{};
    bool backbufferTagged = false;
    Region backbufferExtent{};

    // Last described UI tag (resource description, sampled about once a
    // second). Recomposition requires it to match the HUDless extent.
    uint32_t uiType = 0;
    uint32_t uiFormat = 0;
    uint32_t uiWidth = 0;
    uint32_t uiHeight = 0;
    uint64_t uiDescribedTick = 0;

    // Probe in flight.
    uint64_t generation = 0;
    uint64_t nextArmTick = 0;
    uint64_t stageTick = 0;
    hd::TileLayout layout{};
    Region probeExtent{};
    uint32_t probeFormat = 0;
    // Frame index of the probed HUDless tag. When paired, capture 0 is taken at
    // that frame's Present rather than the next Present.
    uint32_t probeFrame = kNoFrame;
    bool probePaired = false;
    bool lastPaired = false;
    uint32_t lastMotionPermille = 0;
    uint32_t unpairedMotionProbes = 0;
    // Paired probes that matched nothing while the scene moved: the game's
    // HUDless lacks a motion effect (such as motion blur) that still frames hide.
    uint32_t motionDiffersProbes = 0;
    TileBuffer hudless;
    ComPtr<ID3D12Device> hudlessDevice;
    LUID hudlessAdapter{};
    // Tagged without a command list: copied at the next Present. Held only
    // until that capture is recorded.
    ComPtr<ID3D12Resource> deferredResource;
    UINT deferredState = 0;
    DXGI_FORMAT deferredFormat = DXGI_FORMAT_UNKNOWN;
    ComPtr<ID3D12Resource> heldHudless;
    UINT64 heldHudlessFence = 0;

    ComPtr<ID3D12Device> presentDevice;
    ComPtr<ID3D12Fence> fence;
    ComPtr<ID3D12GraphicsCommandList> list;
    std::array<ComPtr<ID3D12CommandAllocator>, kFinalCaptures> allocators;
    std::array<TileBuffer, kFinalCaptures> finals;
    std::array<UINT64, kFinalCaptures> fenceValues{};
    UINT64 nextFence = 1;
    uint32_t captures = 0;
    Region finalRegion{};
    uint32_t finalFormat = 0;
    uint32_t finalWidth = 0;
    uint32_t finalHeight = 0;
    uint32_t colorSpace = 0;

    // Resources a submitted game command list may still reference. They are
    // never released, so a late or repeated execution cannot write freed memory.
    std::vector<ComPtr<ID3D12DeviceChild>> retired;

    // Results.
    hd::Aggregator aggregator;
    hd::ProbeResult last{};
    uint32_t lastOffset = 0;
    hd::Verdict blockingVerdict = hd::Verdict::eNotMeasured;
    const char* reason = "none";
    uint32_t failures = 0;
    hd::Source reportedSource = hd::Source::eWaiting;
    hd::Source policySource = hd::Source::eWaiting;
    uint64_t policySourceTick = 0;
    hd::Route reportedRoute = hd::Route::eUndetermined;
    hd::Verdict reportedVerdict = hd::Verdict::eNotMeasured;
};

State gState;
std::atomic<uint32_t> gStage{static_cast<uint32_t>(Stage::eIdle)};
std::atomic<uint32_t> gApi{static_cast<uint32_t>(Api::eUnknown)};
std::atomic<uintptr_t> gDeviceIdentity{0};
std::atomic<LogSink> gLogSink{nullptr};
std::atomic<bool> gEnabled{true};
std::atomic<bool> gSettingsLoaded{false};
std::array<std::atomic<uint64_t>, kTypeSlots> gTypeCounts{};
std::atomic<uint64_t> gOtherTypes{0};
std::atomic<uint64_t> gFramedCalls{0};
std::atomic<uint64_t> gLegacyCalls{0};
std::atomic<uint64_t> gHudlessWithoutList{0};
std::atomic<uint64_t> gHudlessEmptyTags{0};
std::atomic<uint64_t> gFirstHudlessTick{0};
std::atomic<uint64_t> gLastHudlessTick{0};
std::atomic<uint64_t> gLastUiTick{0};
std::atomic<uint64_t> gFirstInputsTick{0};
// Present pairing: latest ePresentStart frame, latest tagged HUDless frame, and
// how far tags ran ahead of the Present that followed them.
std::atomic<uint32_t> gPresentFrame{0};
std::atomic<uint64_t> gPresentMarkers{0};
std::atomic<uint64_t> gLastPresentMarkerTick{0};
std::atomic<uint32_t> gLastTagFrame{kNoFrame};
std::atomic<int32_t> gTagLead{0};
std::atomic<int32_t> gMaxTagLead{0};
std::wstring gDumpDirectory; // Written once by LoadSettings before any probe.

void Log(const wchar_t* format, ...) noexcept
{
    const LogSink sink = gLogSink.load(std::memory_order_acquire);
    if (!sink) return;
    wchar_t message[768]{};
    va_list args;
    va_start(args, format);
    _vsnwprintf_s(message, _countof(message), _TRUNCATE, format, args);
    va_end(args);
    sink(message);
}

Stage CurrentStage() noexcept
{
    return static_cast<Stage>(gStage.load(std::memory_order_acquire));
}

void SetStage(Stage stage) noexcept
{
    gStage.store(static_cast<uint32_t>(stage), std::memory_order_release);
}

uintptr_t Identity(IUnknown* object) noexcept
{
    if (!object) return 0;
    IUnknown* identity = nullptr;
    if (FAILED(object->QueryInterface(IID_PPV_ARGS(&identity))) || !identity) return 0;
    const auto value = reinterpret_cast<uintptr_t>(identity);
    identity->Release();
    return value;
}

std::atomic<DeviceIdentityResolver> gDeviceResolver{nullptr};

// Devices reached through different objects (queue, list, resource) can be
// different proxy layers over one native device.
uintptr_t NativeDeviceIdentity(ID3D12Device* device) noexcept
{
    if (!device) return 0;
    const auto resolver = gDeviceResolver.load(std::memory_order_acquire);
    const uintptr_t resolved = resolver ? resolver(device) : 0;
    return resolved ? resolved : Identity(device);
}

bool SameNativeDevice(ID3D12Device* a, ID3D12Device* b, const wchar_t* where) noexcept
{
    const uintptr_t left = NativeDeviceIdentity(a);
    const uintptr_t right = NativeDeviceIdentity(b);
    if (left && left == right) return true;
    static std::atomic<bool> logged{false};
    if (!logged.exchange(true, std::memory_order_relaxed))
    {
        const LUID la = a ? a->GetAdapterLuid() : LUID{};
        const LUID lb = b ? b->GetAdapterLuid() : LUID{};
        wchar_t message[400]{};
        swprintf_s(message, L"HUDless probe device mismatch (%s): raw %p/%p identity %p/%p "
            L"native %p/%p luid %08X:%08X/%08X:%08X resolver=%d", where,
            static_cast<void*>(a), static_cast<void*>(b),
            reinterpret_cast<void*>(Identity(a)), reinterpret_cast<void*>(Identity(b)),
            reinterpret_cast<void*>(left), reinterpret_cast<void*>(right),
            static_cast<unsigned>(la.HighPart), la.LowPart,
            static_cast<unsigned>(lb.HighPart), lb.LowPart,
            gDeviceResolver.load(std::memory_order_acquire) != nullptr);
        const LogSink sink = gLogSink.load(std::memory_order_acquire);
        if (sink) sink(message);
    }
    return false;
}

uint64_t Align(uint64_t value, uint64_t alignment) noexcept
{
    return (value + alignment - 1) / alignment * alignment;
}

std::once_flag gSettingsOnce;
std::wstring gIniPath; // Written once under gSettingsOnce.
std::atomic<bool> gRecompositionVerified{false};
std::atomic<uint32_t> gRecompositionState{0};
// Where the verified HUD layer comes from: the game's UI buffer, or final colour
// minus HUDless for games that tag HUDless alone.
enum class UiMode : uint32_t { eNone = 0, eGame, eSynthesized };
std::atomic<uint32_t> gRecompositionMode{0}; // Verified mode, as recorded in the INI.

const char* UiModeName(uint32_t mode) noexcept
{
    switch (static_cast<UiMode>(mode))
    {
    case UiMode::eGame: return "game";
    case UiMode::eSynthesized: return "synthesized";
    default: return "none";
    }
}

// Hypothesis of the latest equivalent probe, used by the debug tint.
std::atomic<uint32_t> gRelation{0};
uint32_t gLoggedRecompositionState = 0; // Worker thread only.

// Settings may be first needed on the game's options thread, before the
// worker's first poll, so loading is once-only and complete before use.
void LoadSettings() noexcept
{
    std::call_once(gSettingsOnce, [] {
        gSettingsLoaded.store(true, std::memory_order_release);
        wchar_t path[MAX_PATH * 4]{};
        const DWORD length = GetModuleFileNameW(nullptr, path, static_cast<DWORD>(std::size(path)));
        if (length && length < std::size(path))
        {
            wchar_t* slash = wcsrchr(path, L'\\');
            if (slash)
            {
                slash[1] = L'\0';
                if (!wcscat_s(path, L"RTX40MFG-UI.ini"))
                    gIniPath = path;
            }
        }
        if (!gIniPath.empty())
        {
            const bool enabled = GetPrivateProfileIntW(L"HUD", L"Detection", 1, gIniPath.c_str()) != 0;
            gEnabled.store(enabled, std::memory_order_release);
            const bool verified = GetPrivateProfileIntW(L"HUD", L"RecompositionVerified", 0, gIniPath.c_str()) != 0;
            // Verifications recorded before RecompositionUi existed were game pairs.
            wchar_t mode[16]{};
            GetPrivateProfileStringW(L"HUD", L"RecompositionUi", L"game", mode,
                static_cast<DWORD>(std::size(mode)), gIniPath.c_str());
            const UiMode stored = _wcsicmp(mode, L"synthesized") == 0 ? UiMode::eSynthesized : UiMode::eGame;
            gRecompositionVerified.store(verified, std::memory_order_release);
            gRecompositionMode.store(static_cast<uint32_t>(verified ? stored : UiMode::eNone),
                std::memory_order_release);
        }
        wchar_t dump[MAX_PATH]{};
        const DWORD dumpLength = GetEnvironmentVariableW(L"RTXMFG_HUDLESS_DUMP", dump, MAX_PATH);
        if (dumpLength && dumpLength < MAX_PATH) gDumpDirectory = dump;
    });
}

void LogSettings() noexcept
{
    static std::atomic<bool> logged{false};
    if (logged.exchange(true, std::memory_order_relaxed)) return;
    if (!gEnabled.load(std::memory_order_acquire))
        Log(L"HUDless detection disabled by [HUD] Detection=0");
    Log(L"HUD layer: verifiedEarlier=%d ui=%hs ini=%s (UI recomposition is left to the game)",
        gRecompositionVerified.load(std::memory_order_acquire),
        UiModeName(gRecompositionMode.load(std::memory_order_acquire)), gIniPath.c_str());
    if (!gDumpDirectory.empty())
        Log(L"HUDless detection diagnostic tile dump enabled: %s", gDumpDirectory.c_str());
}

const char* RecompositionStateName(uint32_t state) noexcept
{
    switch (static_cast<RecompositionState>(state))
    {
    case RecompositionState::eGameManaged: return "game-managed";
    case RecompositionState::eOff: return "off";
    default: return "unknown";
    }
}

// Readback buffers are always in COPY_DEST, as the heap type requires.
bool CreateTileBuffer(ID3D12Device* device, uint32_t format,
    const hd::TileLayout& layout, TileBuffer& output) noexcept
{
    const auto info = hd::Describe(format);
    if (!device || !info.supported || !layout.count) return false;
    TileBuffer buffer{};
    buffer.format = format;
    buffer.bytesPerPixel = info.bytesPerPixel;
    uint64_t offset = 0;
    for (uint32_t index = 0; index < layout.count; ++index)
    {
        const auto& tile = layout.tiles[index];
        offset = Align(offset, D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT);
        buffer.tiles[index].offset = offset;
        buffer.tiles[index].rowPitch = static_cast<uint32_t>(Align(
            uint64_t(tile.width) * info.bytesPerPixel, D3D12_TEXTURE_DATA_PITCH_ALIGNMENT));
        offset += uint64_t(buffer.tiles[index].rowPitch) * tile.height;
    }
    buffer.markerOffset = Align(offset, 256);
    buffer.bytes = buffer.markerOffset + 256;

    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = buffer.bytes;
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
            D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&buffer.buffer))))
        return false;
    output = std::move(buffer);
    return true;
}

bool SameLayout(const TileBuffer& buffer, uint32_t format, const hd::TileLayout& layout) noexcept
{
    if (!buffer.buffer || buffer.format != format) return false;
    const auto info = hd::Describe(format);
    uint64_t offset = 0;
    for (uint32_t index = 0; index < layout.count; ++index)
    {
        offset = Align(offset, D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT);
        if (buffer.tiles[index].offset != offset) return false;
        const auto pitch = static_cast<uint32_t>(Align(uint64_t(layout.tiles[index].width)
            * info.bytesPerPixel, D3D12_TEXTURE_DATA_PITCH_ALIGNMENT));
        if (buffer.tiles[index].rowPitch != pitch) return false;
        offset += uint64_t(pitch) * layout.tiles[index].height;
    }
    return buffer.markerOffset == Align(offset, 256);
}

void Retire(TileBuffer& buffer) noexcept
{
    if (buffer.buffer) gState.retired.push_back(std::move(buffer.buffer));
    buffer = {};
}

void RecordTiles(ID3D12GraphicsCommandList* list, ID3D12Resource* source,
    DXGI_FORMAT sourceFormat, const Region& region, const hd::TileLayout& layout,
    const TileBuffer& target) noexcept
{
    for (uint32_t index = 0; index < layout.count; ++index)
    {
        const auto& tile = layout.tiles[index];
        D3D12_TEXTURE_COPY_LOCATION destination{};
        destination.pResource = target.buffer.Get();
        destination.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        destination.PlacedFootprint.Offset = target.tiles[index].offset;
        destination.PlacedFootprint.Footprint.Format = sourceFormat;
        destination.PlacedFootprint.Footprint.Width = tile.width;
        destination.PlacedFootprint.Footprint.Height = tile.height;
        destination.PlacedFootprint.Footprint.Depth = 1;
        destination.PlacedFootprint.Footprint.RowPitch = target.tiles[index].rowPitch;
        D3D12_TEXTURE_COPY_LOCATION origin{};
        origin.pResource = source;
        origin.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        origin.SubresourceIndex = 0;
        D3D12_BOX box{};
        box.left = region.left + tile.left;
        box.top = region.top + tile.top;
        box.front = 0;
        box.right = box.left + tile.width;
        box.bottom = box.top + tile.height;
        box.back = 1;
        list->CopyTextureRegion(&destination, 0, 0, 0, &origin, &box);
    }
}

D3D12_RESOURCE_BARRIER Transition(ID3D12Resource* resource,
    D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) noexcept
{
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = resource;
    barrier.Transition.Subresource = 0;
    barrier.Transition.StateBefore = before;
    barrier.Transition.StateAfter = after;
    return barrier;
}

void Abandon(const char* reason, hd::Verdict blocking, uint64_t cooldownMs) noexcept
{
    gState.reason = reason;
    if (blocking != hd::Verdict::eNotMeasured) gState.blockingVerdict = blocking;
    ++gState.failures;
    gState.captures = 0;
    gState.deferredResource.Reset();
    gState.nextArmTick = GetTickCount64() + cooldownMs;
    SetStage(Stage::eIdle);
}

// States a compute command list cannot transition.
constexpr UINT kGraphicsOnlyStates = D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER
    | D3D12_RESOURCE_STATE_INDEX_BUFFER | D3D12_RESOURCE_STATE_RENDER_TARGET
    | D3D12_RESOURCE_STATE_DEPTH_WRITE | D3D12_RESOURCE_STATE_DEPTH_READ
    | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_STREAM_OUT
    | D3D12_RESOURCE_STATE_RESOLVE_DEST | D3D12_RESOURCE_STATE_RESOLVE_SOURCE;

// Validates a described HUDless tag and records its description. Returns false
// after abandoning the probe. Caller holds gState.mutex.
bool DescribeHudless(const Tag& tag, ComPtr<ID3D12Resource>& resource,
    D3D12_RESOURCE_DESC& desc, Region& extent, uint32_t& decodeFormat) noexcept
{
    if (FAILED(static_cast<IUnknown*>(tag.native)->QueryInterface(IID_PPV_ARGS(&resource))) || !resource)
    {
        Abandon("HUDless tag is not a D3D12 resource", hd::Verdict::eUnavailable, kSlowCadenceMs);
        return false;
    }
    desc = resource->GetDesc();
    if (desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || desc.SampleDesc.Count != 1
        || desc.Width > UINT32_MAX || !desc.Width || !desc.Height)
    {
        Abandon("HUDless resource shape unsupported", hd::Verdict::eShapeMismatch, kSlowCadenceMs);
        return false;
    }
    const uint32_t width = static_cast<uint32_t>(desc.Width);
    extent = {tag.extentLeft, tag.extentTop,
        tag.extentWidth ? tag.extentWidth : width, tag.extentHeight ? tag.extentHeight : desc.Height};
    if (extent.left > width || extent.width > width - extent.left
        || extent.top > desc.Height || extent.height > desc.Height - extent.top)
    {
        Abandon("HUDless extent outside the resource", hd::Verdict::eShapeMismatch, kSlowCadenceMs);
        return false;
    }
    decodeFormat = hd::RefineFormat(static_cast<uint32_t>(desc.Format), tag.nativeFormat);
    gState.hudlessResourceFormat = static_cast<uint32_t>(desc.Format);
    gState.hudlessFormat = decodeFormat;
    gState.hudlessWidth = width;
    gState.hudlessHeight = desc.Height;
    gState.hudlessLifecycle = tag.lifecycle;
    gState.hudlessState = tag.state;
    gState.hudlessExtent = extent;
    if (!hd::Describe(decodeFormat).supported || !hd::Describe(static_cast<uint32_t>(desc.Format)).supported)
    {
        Abandon("HUDless format not decodable", hd::Verdict::eUnsupportedFormat, kSlowCadenceMs);
        return false;
    }
    if (tag.state == kUnknownState)
    {
        Abandon("HUDless tag has no resource state", hd::Verdict::eUnavailable, kSlowCadenceMs);
        return false;
    }
    return true;
}

bool EnsureHudlessBuffer(ID3D12Device* device, DXGI_FORMAT format, const hd::TileLayout& layout) noexcept
{
    if (gState.hudlessDevice.Get() == device && SameLayout(gState.hudless, static_cast<uint32_t>(format), layout))
        return true;
    Retire(gState.hudless);
    gState.hudlessDevice.Reset();
    if (!CreateTileBuffer(device, static_cast<uint32_t>(format), layout, gState.hudless))
    {
        Abandon("HUDless readback allocation failed", hd::Verdict::eUnavailable, kSlowCadenceMs);
        return false;
    }
    gState.hudlessDevice = device;
    return true;
}

// Transitions subresource 0 only when its tagged state lacks COPY_SOURCE, and
// writes the probe generation after the copies so evaluation can prove the
// copies executed.
void RecordHudlessCopy(ID3D12GraphicsCommandList* list, ID3D12GraphicsCommandList2* markerList,
    ID3D12Resource* resource, UINT taggedState, DXGI_FORMAT format, const Region& extent,
    const hd::TileLayout& layout) noexcept
{
    const auto state = static_cast<D3D12_RESOURCE_STATES>(taggedState);
    const bool transition = (taggedState & D3D12_RESOURCE_STATE_COPY_SOURCE) == 0;
    if (transition)
    {
        const auto barrier = Transition(resource, state, D3D12_RESOURCE_STATE_COPY_SOURCE);
        list->ResourceBarrier(1, &barrier);
    }
    RecordTiles(list, resource, format, extent, layout, gState.hudless);
    D3D12_WRITEBUFFERIMMEDIATE_PARAMETER marker{};
    marker.Dest = gState.hudless.buffer->GetGPUVirtualAddress() + gState.hudless.markerOffset;
    marker.Value = static_cast<UINT>(gState.generation);
    const D3D12_WRITEBUFFERIMMEDIATE_MODE mode = D3D12_WRITEBUFFERIMMEDIATE_MODE_MARKER_OUT;
    markerList->WriteBufferImmediate(1, &marker, &mode);
    if (transition)
    {
        const auto barrier = Transition(resource, D3D12_RESOURCE_STATE_COPY_SOURCE, state);
        list->ResourceBarrier(1, &barrier);
    }
}

void BeginCapture(const Region& extent, const hd::TileLayout& layout, uint32_t decodeFormat, Stage stage,
    uint32_t frame) noexcept
{
    // Tags normally run 0-2 frames ahead of the last Present; anything outside
    // a small window means the two frame counters are not comparable.
    constexpr int32_t kMaxPairedLead = 8;
    uint32_t present = 0;
    gState.probeFrame = frame;
    const bool presentKnown = frame != kNoFrame && CurrentPresentFrame(present);
    const int32_t ahead = static_cast<int32_t>(frame - present);
    gState.probePaired = presentKnown && ahead >= 0 && ahead <= kMaxPairedLead;
    gState.layout = layout;
    gState.probeExtent = extent;
    gState.probeFormat = decodeFormat;
    gState.captures = 0;
    gState.stageTick = GetTickCount64();
    SetStage(stage);
}

// Tag-time path for eOnlyValidNow: copies the tiles on the tagging command
// list, as Streamline does. Caller holds gState.mutex and the stage is Armed.
void RecordHudless(const Tag& tag, void* commandList, uint32_t frame) noexcept
{
    ComPtr<ID3D12GraphicsCommandList> list;
    ComPtr<ID3D12GraphicsCommandList2> markerList;
    if (FAILED(static_cast<IUnknown*>(commandList)->QueryInterface(IID_PPV_ARGS(&list))) || !list)
        return Abandon("tagging command list is not a D3D12 graphics list", hd::Verdict::eNotMeasured, kSlowCadenceMs);
    const auto type = list->GetType();
    if (type != D3D12_COMMAND_LIST_TYPE_DIRECT && type != D3D12_COMMAND_LIST_TYPE_COMPUTE)
        return Abandon("HUDless tagged on an unsupported command list type", hd::Verdict::eUnavailable, kSlowCadenceMs);
    if (FAILED(list.As(&markerList)))
        return Abandon("command list lacks WriteBufferImmediate", hd::Verdict::eUnavailable, kSlowCadenceMs);

    ComPtr<ID3D12Resource> resource;
    D3D12_RESOURCE_DESC desc{};
    Region extent{};
    uint32_t decodeFormat = 0;
    if (!DescribeHudless(tag, resource, desc, extent, decodeFormat)) return;

    ComPtr<ID3D12Device> listDevice, resourceDevice;
    if (FAILED(list->GetDevice(IID_PPV_ARGS(&listDevice)))
        || FAILED(resource->GetDevice(IID_PPV_ARGS(&resourceDevice)))
        || !SameNativeDevice(listDevice.Get(), resourceDevice.Get(), L"tag list"))
        return Abandon("HUDless resource and command list devices differ", hd::Verdict::eUnavailable, kSlowCadenceMs);
    if (type == D3D12_COMMAND_LIST_TYPE_COMPUTE && (tag.state & kGraphicsOnlyStates))
        return Abandon("HUDless state cannot be transitioned on a compute list", hd::Verdict::eUnavailable, kSlowCadenceMs);

    const hd::TileLayout layout = hd::Layout(extent.width, extent.height);
    if (!EnsureHudlessBuffer(listDevice.Get(), desc.Format, layout)) return;
    gState.hudlessAdapter = listDevice->GetAdapterLuid();
    gState.deferredResource.Reset();
    RecordHudlessCopy(list.Get(), markerList.Get(), resource.Get(), tag.state, desc.Format, extent, layout);
    BeginCapture(extent, layout, decodeFormat, Stage::eHudlessRecorded, frame);
}

// Present-time path for eValidUntilPresent/eValidUntilEvaluate. The tagged
// state is the state at use, which Streamline requires for these lifecycles.
void DeferHudless(const Tag& tag, uint32_t frame) noexcept
{
    constexpr uint32_t kValidUntilPresent = 1;
    constexpr uint32_t kValidUntilEvaluate = 2;
    if (tag.lifecycle != kValidUntilPresent && tag.lifecycle != kValidUntilEvaluate)
        return Abandon("HUDless lifecycle unsupported", hd::Verdict::eUnavailable, kSlowCadenceMs);
    ComPtr<ID3D12Resource> resource;
    D3D12_RESOURCE_DESC desc{};
    Region extent{};
    uint32_t decodeFormat = 0;
    if (!DescribeHudless(tag, resource, desc, extent, decodeFormat)) return;
    ComPtr<ID3D12Device> resourceDevice;
    if (FAILED(resource->GetDevice(IID_PPV_ARGS(&resourceDevice))))
        return Abandon("HUDless resource device unavailable", hd::Verdict::eUnavailable, kSlowCadenceMs);
    gState.hudlessAdapter = resourceDevice->GetAdapterLuid();
    gState.deferredResource = resource;
    gState.deferredState = tag.state;
    gState.deferredFormat = desc.Format;
    BeginCapture(extent, hd::Layout(extent.width, extent.height), decodeFormat, Stage::eHudlessDeferred, frame);
}
bool EnsurePresentResources(ID3D12Device* device, uint32_t format) noexcept
{
    if (gState.presentDevice.Get() != device)
    {
        // A previous device's capture may still be executing.
        for (auto& buffer : gState.finals) Retire(buffer);
        if (gState.fence) gState.retired.push_back(std::move(gState.fence));
        if (gState.list) gState.retired.push_back(std::move(gState.list));
        for (auto& allocator : gState.allocators)
            if (allocator) gState.retired.push_back(std::move(allocator));
        if (gState.heldHudless) gState.retired.push_back(std::move(gState.heldHudless));
        gState.presentDevice.Reset();
        gState.fenceValues = {};
        gState.nextFence = 1;
        for (auto& allocator : gState.allocators)
            if (FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator))))
                return false;
        if (FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&gState.fence)))
            || FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                gState.allocators[0].Get(), nullptr, IID_PPV_ARGS(&gState.list)))
            || FAILED(gState.list->Close()))
            return false;
        gState.presentDevice = device;
    }
    for (auto& buffer : gState.finals)
    {
        if (SameLayout(buffer, format, gState.layout)) continue;
        Retire(buffer);
        if (!CreateTileBuffer(device, format, gState.layout, buffer)) return false;
    }
    return true;
}

void DecodeInto(hd::Statistics& statistics, const uint8_t* hudless, const TileBuffer& hudlessTiles,
    uint32_t hudlessFormat, const uint8_t* final, const TileBuffer& finalTiles, uint32_t finalFormat,
    const hd::TileLayout& layout) noexcept
{
    std::array<float, hd::kTileSize * hd::kTileSize * 3> tileHudless{}, tileFinal{};
    for (uint32_t index = 0; index < layout.count; ++index)
    {
        const auto& tile = layout.tiles[index];
        uint32_t count = 0;
        for (uint32_t y = 0; y < tile.height; ++y)
        {
            const uint8_t* h = hudless + hudlessTiles.tiles[index].offset
                + uint64_t(y) * hudlessTiles.tiles[index].rowPitch;
            const uint8_t* f = final + finalTiles.tiles[index].offset
                + uint64_t(y) * finalTiles.tiles[index].rowPitch;
            for (uint32_t x = 0; x < tile.width; ++x)
            {
                float* a = &tileHudless[3 * count];
                float* b = &tileFinal[3 * count];
                if (!hd::Decode(hudlessFormat, h + uint64_t(x) * hudlessTiles.bytesPerPixel, a)
                    || !hd::Decode(finalFormat, f + uint64_t(x) * finalTiles.bytesPerPixel, b))
                    continue;
                hd::Accumulate(statistics, hudlessFormat, a, finalFormat, b);
                ++count;
            }
        }
        hd::AccumulateTile(statistics, hudlessFormat, tileHudless.data(), finalFormat,
            tileFinal.data(), count);
    }
}

// Caller holds gState.mutex; both captures' fences have completed.
// Opt-in diagnostic (RTXMFG_HUDLESS_DUMP=<directory>): writes the first probes'
// tiles unpadded, in layout order, with a one-line text header.
void DumpTiles(uint32_t probe, const wchar_t* name, const uint8_t* data,
    const TileBuffer& tiles, uint32_t format) noexcept
{
    if (gDumpDirectory.empty() || probe > 16 || !data) return;
    wchar_t path[MAX_PATH * 2]{};
    swprintf_s(path, L"%s\\probe-%02u-%s.bin", gDumpDirectory.c_str(), probe, name);
    FILE* file = nullptr;
    if (_wfopen_s(&file, path, L"wb") || !file) return;
    char header[160]{};
    const int length = sprintf_s(header, "HUDLESS-TILES format=%u bpp=%u tiles=%u tile=%ux%u\n",
        format, tiles.bytesPerPixel, gState.layout.count,
        gState.layout.count ? gState.layout.tiles[0].width : 0,
        gState.layout.count ? gState.layout.tiles[0].height : 0);
    if (length > 0) fwrite(header, 1, static_cast<size_t>(length), file);
    for (uint32_t index = 0; index < gState.layout.count; ++index)
    {
        const auto& tile = gState.layout.tiles[index];
        for (uint32_t y = 0; y < tile.height; ++y)
            fwrite(data + tiles.tiles[index].offset + uint64_t(y) * tiles.tiles[index].rowPitch,
                1, size_t(tile.width) * tiles.bytesPerPixel, file);
    }
    fclose(file);
}

bool Evaluate(uint64_t now) noexcept
{
    const D3D12_RANGE read{0, static_cast<SIZE_T>(gState.hudless.bytes)};
    const D3D12_RANGE none{0, 0};
    uint8_t* hudless = nullptr;
    if (FAILED(gState.hudless.buffer->Map(0, &read, reinterpret_cast<void**>(&hudless))) || !hudless)
    {
        Abandon("HUDless readback map failed", hd::Verdict::eNotMeasured, kSlowCadenceMs);
        return true;
    }
    uint32_t marker = 0;
    std::memcpy(&marker, hudless + gState.hudless.markerOffset, sizeof(marker));
    if (marker != static_cast<uint32_t>(gState.generation))
    {
        gState.hudless.buffer->Unmap(0, &none);
        if (now - gState.stageTick < kStageTimeoutMs) return false;
        Abandon("tagging command list did not execute the HUDless copy", hd::Verdict::eNotMeasured,
            kFastCadenceMs);
        return true;
    }

    const uint32_t probe = gState.aggregator.Probes() + 1;
    DumpTiles(probe, L"hudless", hudless, gState.hudless, gState.probeFormat);
    std::array<hd::ProbeResult, kFinalCaptures> results{};
    std::array<uint8_t*, kFinalCaptures> finals{};
    for (uint32_t capture = 0; capture < kFinalCaptures; ++capture)
    {
        const D3D12_RANGE finalRead{0, static_cast<SIZE_T>(gState.finals[capture].bytes)};
        if (FAILED(gState.finals[capture].buffer->Map(0, &finalRead, reinterpret_cast<void**>(&finals[capture]))))
            finals[capture] = nullptr;
        if (!finals[capture]) continue;
        DumpTiles(probe, capture ? L"final1" : L"final0", finals[capture], gState.finals[capture], gState.finalFormat);
        hd::Statistics statistics{};
        DecodeInto(statistics, hudless, gState.hudless, gState.probeFormat,
            finals[capture], gState.finals[capture], gState.finalFormat, gState.layout);
        results[capture] = hd::Evaluate(statistics);
    }
    // Motion between the two captured Presents. Without frame pairing, a
    // HUDless image cannot be matched to its Present while the scene moves.
    uint32_t motionPermille = 1000;
    if (finals[0] && finals[1])
    {
        hd::Statistics motion{};
        DecodeInto(motion, finals[0], gState.finals[0], gState.finalFormat,
            finals[1], gState.finals[1], gState.finalFormat, gState.layout);
        const hd::ProbeResult still = hd::Evaluate(motion);
        if (still.outcome != hd::Outcome::eInsufficientSignal) motionPermille = still.identityPermille;
    }
    for (uint32_t capture = 0; capture < kFinalCaptures; ++capture)
        if (finals[capture]) gState.finals[capture].buffer->Unmap(0, &none);
    gState.hudless.buffer->Unmap(0, &none);

    uint32_t chosen = 0;
    const bool paired = gState.probePaired;
    const hd::ProbeResult best = paired ? results[0] : hd::Better(results[0], results[1], chosen);
    // A probe that matched nothing while the scene moved decides nothing:
    // unpaired, its Present may be another frame; paired, the game's HUDless may
    // lack a motion effect such as motion blur. Still frames decide the verdict.
    constexpr uint32_t kStillPermille = 900;
    const bool movingMismatch = motionPermille < kStillPermille && best.outcome != hd::Outcome::eEquivalent;
    hd::ProbeResult counted = best;
    if (movingMismatch)
    {
        counted.outcome = hd::Outcome::eInsufficientSignal; // Recorded, never counted.
        ++(paired ? gState.motionDiffersProbes : gState.unpairedMotionProbes);
    }
    gState.aggregator.Add(counted);
    gState.last = best;
    gState.lastPaired = paired;
    gState.lastMotionPermille = motionPermille;
    if (best.outcome == hd::Outcome::eEquivalent)
        gRelation.store(static_cast<uint32_t>(best.bestHypothesis), std::memory_order_release);
    gState.lastOffset = chosen;
    gState.blockingVerdict = hd::Verdict::eNotMeasured;
    gState.reason = "none";
    gState.captures = 0;
    gState.nextArmTick = now + (gState.aggregator.Probes() < kFastProbes ? kFastCadenceMs : kSlowCadenceMs);
    SetStage(Stage::eIdle);
    Log(L"HUDless probe %u: outcome=%hs identity=%u composite=%u tiles=%u translucent=%u "
        L"best=%hs:%u signal=%u/%u offset=%u paired=%d frame=%u lead=%d still=%u%hs "
        L"[capture0 %hs %u, capture1 %hs %u] hudless=%ux%u fmt=%u final=%ux%u fmt=%u verdict=%hs",
        gState.aggregator.Probes(), hd::OutcomeName(best.outcome), best.identityPermille,
        best.compositePermille, best.informativeTiles, best.translucentTiles,
        hd::HypothesisName(best.bestHypothesis), best.bestPermille, best.signalSamples, best.samples,
        chosen, paired, gState.probeFrame, gTagLead.load(std::memory_order_relaxed), motionPermille,
        !movingMismatch ? "" : paired ? " (differs while moving: not counted)"
            : " (unpaired while moving: not counted)",
        hd::OutcomeName(results[0].outcome), results[0].bestPermille,
        hd::OutcomeName(results[1].outcome), results[1].bestPermille,
        gState.probeExtent.width, gState.probeExtent.height, gState.probeFormat,
        gState.finalRegion.width, gState.finalRegion.height, gState.finalFormat,
        hd::VerdictName(gState.aggregator.Current()));
    return true;
}

// Records the UI tag's effective size from its D3D12 description. Engines
// may leave the Streamline resource header zero, so the header is not used.
void DescribeUi(const Tag& tag, uint64_t now) noexcept
{
    {
        std::lock_guard lock(gState.mutex);
        if (gState.uiDescribedTick && now - gState.uiDescribedTick < kUiDescribeMs) return;
        gState.uiDescribedTick = now;
    }
    ComPtr<ID3D12Resource> resource;
    if (FAILED(static_cast<IUnknown*>(tag.native)->QueryInterface(IID_PPV_ARGS(&resource))) || !resource)
        return;
    const D3D12_RESOURCE_DESC desc = resource->GetDesc();
    if (desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || desc.Width > UINT32_MAX) return;
    const uint32_t width = tag.extentWidth ? tag.extentWidth : static_cast<uint32_t>(desc.Width);
    const uint32_t height = tag.extentHeight ? tag.extentHeight : desc.Height;
    const bool inside = tag.extentLeft <= desc.Width && width <= desc.Width - tag.extentLeft
        && tag.extentTop <= desc.Height && height <= desc.Height - tag.extentTop;
    std::lock_guard lock(gState.mutex);
    gState.uiType = tag.type;
    gState.uiFormat = static_cast<uint32_t>(desc.Format);
    gState.uiWidth = inside ? width : 0;
    gState.uiHeight = inside ? height : 0;
}

hd::Source CurrentSource(uint64_t now) noexcept
{
    const uint64_t firstHudless = gFirstHudlessTick.load(std::memory_order_acquire);
    const uint64_t lastUi = gLastUiTick.load(std::memory_order_acquire);
    const uint64_t lastHudless = gLastHudlessTick.load(std::memory_order_acquire);
    const uint64_t firstInputs = gFirstInputsTick.load(std::memory_order_acquire);
    if (firstHudless)
    {
        const bool uiWithHudless = lastUi && lastHudless
            && (lastUi > lastHudless ? lastUi - lastHudless : lastHudless - lastUi) <= kRecentMs;
        return uiWithHudless ? hd::Source::eGameHudlessAndUi : hd::Source::eGameHudless;
    }
    if (lastUi) return hd::Source::eGameUiOnly;
    if (firstInputs && now >= firstInputs && now - firstInputs >= kNoHudlessDecisionMs)
        return hd::Source::eNoHudlessTag;
    return hd::Source::eWaiting;
}

hd::Verdict CurrentVerdict() noexcept
{
    const hd::Verdict aggregate = gState.aggregator.Current();
    if (aggregate == hd::Verdict::eEquivalent || aggregate == hd::Verdict::eEncodingDiffers
        || aggregate == hd::Verdict::eContentDiffers)
        return aggregate;
    if (static_cast<Api>(gApi.load(std::memory_order_acquire)) == Api::eVulkan)
        return hd::Verdict::eUnavailable;
    if (gState.blockingVerdict != hd::Verdict::eNotMeasured) return gState.blockingVerdict;
    return aggregate;
}

// Records a verified game pair for later launches and withdraws it when this
// session's evidence contradicts it. Caller holds gState.mutex (worker thread).
void UpdateRecompositionPolicy(hd::Source source, hd::Verdict verdict, bool sourceSettled) noexcept
{
    // A game HUD layer is the UI buffer over HUDless, pixel for pixel.
    const bool uiKnown = gState.uiWidth && gState.uiHeight;
    const bool uiMatches = uiKnown && gState.uiWidth == gState.hudlessExtent.width
        && gState.uiHeight == gState.hudlessExtent.height;
    const bool equivalent = verdict == hd::Verdict::eEquivalent;
    const bool verifiedPair = source == hd::Source::eGameHudlessAndUi && equivalent && uiMatches;
    // Synthesis derives UI alpha from final colour minus HUDless: both must use
    // the same encoding, and each HUDless must be paired with its own Present.
    uint32_t present = 0;
    const bool pairing = gLastTagFrame.load(std::memory_order_acquire) == kNoFrame || CurrentPresentFrame(present);
    const bool synthesisCapable = pairing && hudless_visualizer::SynthesisSupported()
        && static_cast<Api>(gApi.load(std::memory_order_acquire)) == Api::eD3D12
        && gRelation.load(std::memory_order_acquire) == 0;
    // Only a game that never tags a UI buffer needs one synthesized; a game whose
    // UI tags pause (cutscenes) keeps its own.
    const bool verifiedSynthesis = sourceSettled && source == hd::Source::eGameHudless && equivalent
        && synthesisCapable && gLastUiTick.load(std::memory_order_acquire) == 0;
    const UiMode verifiedMode = verifiedPair ? UiMode::eGame
        : verifiedSynthesis ? UiMode::eSynthesized : UiMode::eNone;
    const UiMode stored = static_cast<UiMode>(gRecompositionMode.load(std::memory_order_acquire));
    // A game-pair verification is contradicted when the game stops tagging a UI
    // buffer; a synthesized one only when HUDless itself disappears.
    const bool sourceContradicts = sourceSettled && (source == hd::Source::eGameUiOnly
        || source == hd::Source::eNoHudlessTag
        || (stored == UiMode::eGame && source == hd::Source::eGameHudless));
    const bool contradicted = verdict == hd::Verdict::eEncodingDiffers
        || verdict == hd::Verdict::eContentDiffers || verdict == hd::Verdict::eShapeMismatch
        || verdict == hd::Verdict::eUnsupportedFormat || sourceContradicts;
    if (verifiedMode != UiMode::eNone && verifiedMode != stored && !gIniPath.empty())
    {
        const wchar_t* modeName = verifiedMode == UiMode::eGame ? L"game" : L"synthesized";
        if (WritePrivateProfileStringW(L"HUD", L"RecompositionUi", modeName, gIniPath.c_str())
            && WritePrivateProfileStringW(L"HUD", L"RecompositionVerified", L"1", gIniPath.c_str()))
        {
            gRecompositionMode.store(static_cast<uint32_t>(verifiedMode), std::memory_order_release);
            gRecompositionVerified.store(true, std::memory_order_release);
            Log(L"HUD recomposition: %ls verified; recorded for the next launch",
                verifiedMode == UiMode::eGame ? L"game HUDless/UI pair" : L"HUDless for a synthesized UI buffer");
        }
    }
    else if (verifiedMode == UiMode::eNone && contradicted && stored != UiMode::eNone && !gIniPath.empty())
    {
        if (WritePrivateProfileStringW(L"HUD", L"RecompositionVerified", L"0", gIniPath.c_str()))
        {
            gRecompositionMode.store(static_cast<uint32_t>(UiMode::eNone), std::memory_order_release);
            gRecompositionVerified.store(false, std::memory_order_release);
            Log(L"HUD recomposition: earlier verification withdrawn (source=%hs verdict=%hs)",
                hd::SourceName(source), hd::VerdictName(verdict));
        }
    }
    const uint32_t state = gRecompositionState.load(std::memory_order_acquire);
    if (state != gLoggedRecompositionState)
    {
        gLoggedRecompositionState = state;
        Log(L"UI recomposition (left to the game): %hs", RecompositionStateName(state));
    }
}

void PublishClassification(uint64_t now) noexcept
{
    const hd::Source source = CurrentSource(now);
    const hd::Verdict verdict = CurrentVerdict();
    const hd::Route route = hd::Classify(source, verdict);
    if (source != gState.policySource)
    {
        gState.policySource = source;
        gState.policySourceTick = now;
    }
    UpdateRecompositionPolicy(source, verdict, now - gState.policySourceTick >= gSourceSettledMs);
    if (source != gState.reportedSource || verdict != gState.reportedVerdict
        || route != gState.reportedRoute)
    {
        gState.reportedSource = source;
        gState.reportedVerdict = verdict;
        gState.reportedRoute = route;
        Log(L"HUDless detection: source=%hs verdict=%hs route=%hs reason=%hs",
            hd::SourceName(source), hd::VerdictName(verdict), hd::RouteName(route), gState.reason);
    }
}
}

void SetLogSink(LogSink sink) noexcept
{
    gLogSink.store(sink, std::memory_order_release);
}

void SetDeviceIdentityResolver(DeviceIdentityResolver resolver) noexcept
{
    gDeviceResolver.store(resolver, std::memory_order_release);
}

bool SameNativeDevice(ID3D12Device* a, ID3D12Device* b) noexcept
{
    return SameNativeDevice(a, b, L"external");
}

uint32_t HudlessRelation() noexcept
{
    return gRelation.load(std::memory_order_acquire);
}

void ReportRecomposition(RecompositionState state) noexcept
{
    gRecompositionState.store(static_cast<uint32_t>(state), std::memory_order_release);
}

void ObserveD3D12Device(void* device) noexcept
{
    const uintptr_t identity = Identity(static_cast<IUnknown*>(device));
    if (!identity) return;
    const uintptr_t previous = gDeviceIdentity.exchange(identity, std::memory_order_acq_rel);
    gApi.store(static_cast<uint32_t>(Api::eD3D12), std::memory_order_release);
    if (previous && previous != identity)
    {
        std::lock_guard lock(gState.mutex);
        gState.aggregator.Reset();
        gState.blockingVerdict = hd::Verdict::eNotMeasured;
        gState.captures = 0;
        SetStage(Stage::eIdle);
    }
}

void ObserveVulkan() noexcept
{
    gDeviceIdentity.store(0, std::memory_order_release);
    gApi.store(static_cast<uint32_t>(Api::eVulkan), std::memory_order_release);
    std::lock_guard lock(gState.mutex);
    gState.reason = "Vulkan HUDless detection is not implemented";
    SetStage(Stage::eIdle);
}

void ObserveTags(const Tag* tags, uint32_t count, void* commandList, bool framed, uint32_t frame) noexcept
{
    if (!tags || !count) return;
    (framed ? gFramedCalls : gLegacyCalls).fetch_add(1, std::memory_order_relaxed);
    const uint64_t now = GetTickCount64();
    const Tag* hudless = nullptr;
    const Tag* ui = nullptr;
    for (uint32_t index = 0; index < count; ++index)
    {
        const Tag& tag = tags[index];
        if (tag.type < kTypeSlots) gTypeCounts[tag.type].fetch_add(1, std::memory_order_relaxed);
        else gOtherTypes.fetch_add(1, std::memory_order_relaxed);
        if (!tag.native && !tag.present)
        {
            // Some engines (Unreal's Streamline plugin) tag HUDless without a
            // resource while supplying a real UI buffer.
            if (tag.type == kTypeHudless) gHudlessEmptyTags.fetch_add(1, std::memory_order_relaxed);
            continue;
        }
        if (tag.type == kTypeHudless)
        {
            hudless = &tag;
            uint64_t expected = 0;
            gFirstHudlessTick.compare_exchange_strong(expected, now, std::memory_order_acq_rel);
            gLastHudlessTick.store(now, std::memory_order_release);
        }
        else if (tag.type == kTypeUiAlpha || tag.type == kTypeUiColorAndAlpha)
        {
            gLastUiTick.store(now, std::memory_order_release);
            if (!ui || tag.type == kTypeUiColorAndAlpha) ui = &tag;
        }
        else if (tag.type == kTypeDepth || tag.type == kTypeMotionVectors)
        {
            uint64_t expected = 0;
            gFirstInputsTick.compare_exchange_strong(expected, now, std::memory_order_acq_rel);
        }
        else if (tag.type == kTypeBackbuffer && tag.described)
        {
            std::lock_guard lock(gState.mutex);
            gState.backbufferTagged = tag.extentWidth && tag.extentHeight;
            gState.backbufferExtent = {tag.extentLeft, tag.extentTop, tag.extentWidth, tag.extentHeight};
        }
    }
    // Native handles reach COM only on an established D3D12 device.
    const bool d3d12 = static_cast<Api>(gApi.load(std::memory_order_acquire)) == Api::eD3D12
        && gDeviceIdentity.load(std::memory_order_acquire);
    if (d3d12 && ui && ui->described && ui->native) DescribeUi(*ui, now);
    if (!hudless) return;
    if (frame != kNoFrame) gLastTagFrame.store(frame, std::memory_order_release);
    if (!commandList) gHudlessWithoutList.fetch_add(1, std::memory_order_relaxed);
    if (!d3d12) return;
    if (hudless->described && hudless->native && hudless_visualizer::WantsHudless())
        hudless_visualizer::ObserveHudless(hudless->native, hudless->state, hudless->lifecycle,
            hudless->extentLeft, hudless->extentTop, hudless->extentWidth, hudless->extentHeight, commandList,
            frame);
    if (CurrentStage() != Stage::eArmed) return;
    std::lock_guard lock(gState.mutex);
    if (CurrentStage() != Stage::eArmed) return;
    if (!hudless->described || !hudless->native)
        return Abandon("HUDless tag structure not verified", hd::Verdict::eUnavailable, kSlowCadenceMs);
    // Contents are read when Streamline reads them: at tagging for
    // eOnlyValidNow, otherwise at Present. Games commonly tag valid-until-Present
    // buffers before rendering into them.
    constexpr uint32_t kOnlyValidNow = 0;
    if (hudless->lifecycle != kOnlyValidNow) DeferHudless(*hudless, frame);
    else if (commandList) RecordHudless(*hudless, commandList, frame);
    else Abandon("HUDless valid only at tagging but tagged without a command list",
        hd::Verdict::eUnavailable, kSlowCadenceMs);
}

void ObservePresentFrame(uint32_t frame) noexcept
{
    gPresentFrame.store(frame, std::memory_order_release);
    gLastPresentMarkerTick.store(GetTickCount64(), std::memory_order_release);
    gPresentMarkers.fetch_add(1, std::memory_order_relaxed);
    const uint32_t tagged = gLastTagFrame.load(std::memory_order_acquire);
    if (tagged == kNoFrame) return;
    const int32_t lead = static_cast<int32_t>(tagged - frame);
    gTagLead.store(lead, std::memory_order_relaxed);
    int32_t maximum = gMaxTagLead.load(std::memory_order_relaxed);
    while (lead > maximum && lead < 64
        && !gMaxTagLead.compare_exchange_weak(maximum, lead, std::memory_order_relaxed)) {}
}

bool CurrentPresentFrame(uint32_t& frame) noexcept
{
    const uint64_t tick = gLastPresentMarkerTick.load(std::memory_order_acquire);
    if (!tick || GetTickCount64() - tick > kRecentMs) return false;
    frame = gPresentFrame.load(std::memory_order_acquire);
    return true;
}

bool WantsPresentCapture() noexcept
{
    const Stage stage = CurrentStage();
    return stage == Stage::eHudlessRecorded || stage == Stage::eHudlessDeferred
        || stage == Stage::eCapturing;
}

void CapturePresent(ID3D12Device* device, ID3D12CommandQueue* queue,
    ID3D12Resource* backbuffer, uint32_t colorSpace) noexcept
{
    if (!device || !queue || !backbuffer || !WantsPresentCapture()) return;
    std::lock_guard lock(gState.mutex);
    const Stage stage = CurrentStage();
    if (stage != Stage::eHudlessRecorded && stage != Stage::eHudlessDeferred
        && stage != Stage::eCapturing) return;
    const bool deferred = stage == Stage::eHudlessDeferred;

    const LUID adapter = device->GetAdapterLuid();
    if (adapter.LowPart != gState.hudlessAdapter.LowPart || adapter.HighPart != gState.hudlessAdapter.HighPart)
        return Abandon("presentation device is on a different adapter", hd::Verdict::eUnavailable, kSlowCadenceMs);
    const D3D12_RESOURCE_DESC desc = backbuffer->GetDesc();
    if (desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || desc.Width > UINT32_MAX
        || desc.SampleDesc.Count != 1)
        return Abandon("backbuffer shape unsupported", hd::Verdict::eShapeMismatch, kSlowCadenceMs);
    const uint32_t width = static_cast<uint32_t>(desc.Width);
    Region region{0, 0, width, desc.Height};
    const auto& tagged = gState.backbufferExtent;
    if (gState.backbufferTagged && tagged.width == gState.probeExtent.width
        && tagged.height == gState.probeExtent.height
        && tagged.left <= width && tagged.width <= width - tagged.left
        && tagged.top <= desc.Height && tagged.height <= desc.Height - tagged.top)
        region = tagged;
    gState.finalWidth = width;
    gState.finalHeight = desc.Height;
    gState.finalFormat = static_cast<uint32_t>(desc.Format);
    gState.finalRegion = region;
    gState.colorSpace = colorSpace;
    if (region.width != gState.probeExtent.width || region.height != gState.probeExtent.height)
        return Abandon("HUDless size differs from final colour", hd::Verdict::eShapeMismatch, kSlowCadenceMs);
    if (!hd::Describe(gState.finalFormat).supported)
        return Abandon("final colour format not decodable", hd::Verdict::eUnsupportedFormat, kSlowCadenceMs);
    if (!EnsurePresentResources(device, gState.finalFormat))
        return Abandon("presentation readback allocation failed", hd::Verdict::eUnavailable, kSlowCadenceMs);
    ComPtr<ID3D12GraphicsCommandList2> markerList;
    if (deferred)
    {
        ComPtr<ID3D12Device> resourceDevice;
        if (Identity(gState.deferredResource.Get()) == Identity(backbuffer))
            return Abandon("HUDless tag is the presented buffer", hd::Verdict::eUnavailable, kSlowCadenceMs);
        if (FAILED(gState.deferredResource->GetDevice(IID_PPV_ARGS(&resourceDevice)))
            || !SameNativeDevice(resourceDevice.Get(), device, L"present"))
            return Abandon("HUDless resource is not on the presentation device", hd::Verdict::eUnavailable, kSlowCadenceMs);
        if (FAILED(gState.list.As(&markerList)))
            return Abandon("capture list lacks WriteBufferImmediate", hd::Verdict::eUnavailable, kSlowCadenceMs);
        if (!EnsureHudlessBuffer(device, gState.deferredFormat, gState.layout)) return;
    }

    const uint32_t capture = gState.captures;
    if (capture == 0 && gState.probePaired)
    {
        uint32_t present = 0;
        if (!CurrentPresentFrame(present))
            return Abandon("present markers stopped during a paired probe", hd::Verdict::eNotMeasured, kFastCadenceMs);
        const int32_t distance = static_cast<int32_t>(present - gState.probeFrame);
        if (distance < 0) return; // This HUDless belongs to a later Present.
        if (distance > 0)
            return Abandon("paired Present passed before capture", hd::Verdict::eNotMeasured, kFastCadenceMs);
    }
    if (gState.fence->GetCompletedValue() < gState.fenceValues[capture]) return;
    auto& allocator = gState.allocators[capture];
    if (FAILED(allocator->Reset()) || FAILED(gState.list->Reset(allocator.Get(), nullptr)))
        return Abandon("presentation capture reset failed", hd::Verdict::eUnavailable, kSlowCadenceMs);
    if (deferred)
    {
        RecordHudlessCopy(gState.list.Get(), markerList.Get(), gState.deferredResource.Get(),
            gState.deferredState, gState.deferredFormat, gState.probeExtent, gState.layout);
    }
    auto barrier = Transition(backbuffer, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_SOURCE);
    gState.list->ResourceBarrier(1, &barrier);
    RecordTiles(gState.list.Get(), backbuffer, desc.Format, region, gState.layout, gState.finals[capture]);
    barrier = Transition(backbuffer, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_PRESENT);
    gState.list->ResourceBarrier(1, &barrier);
    if (FAILED(gState.list->Close()))
        return Abandon("presentation capture close failed", hd::Verdict::eUnavailable, kSlowCadenceMs);
    ID3D12CommandList* lists[] = {gState.list.Get()};
    queue->ExecuteCommandLists(1, lists);
    const UINT64 value = gState.nextFence++;
    gState.fenceValues[capture] = value;
    if (FAILED(queue->Signal(gState.fence.Get(), value)))
    {
        // The submitted copy may still run; keep its readback buffer alive.
        Retire(gState.finals[capture]);
        return Abandon("presentation fence signal failed", hd::Verdict::eUnavailable, kSlowCadenceMs);
    }
    // Command lists do not own resources; keep the game's HUDless texture
    // alive until this capture's fence completes.
    if (deferred)
    {
        gState.heldHudless = std::move(gState.deferredResource);
        gState.heldHudlessFence = value;
    }
    gState.captures = capture + 1;
    SetStage(gState.captures >= kFinalCaptures ? Stage::eSubmitted : Stage::eCapturing);
}

void Poll() noexcept
{
    LoadSettings();
    LogSettings();
    const uint64_t now = GetTickCount64();
    std::lock_guard lock(gState.mutex);
    if (gState.heldHudless && gState.fence
        && gState.fence->GetCompletedValue() >= gState.heldHudlessFence)
        gState.heldHudless.Reset();
    if (gEnabled.load(std::memory_order_acquire))
    {
        switch (CurrentStage())
        {
        case Stage::eIdle:
        {
            const uint64_t lastHudless = gLastHudlessTick.load(std::memory_order_acquire);
            if (static_cast<Api>(gApi.load(std::memory_order_acquire)) == Api::eD3D12
                && now >= gState.nextArmTick && lastHudless && now - lastHudless <= kRecentMs)
            {
                ++gState.generation;
                gState.stageTick = now;
                SetStage(Stage::eArmed);
            }
            break;
        }
        case Stage::eArmed:
            if (now - gState.stageTick > kStageTimeoutMs)
                Abandon(gHudlessWithoutList.load(std::memory_order_relaxed)
                        ? "HUDless tagged without a command list" : "no HUDless tag while armed",
                    hd::Verdict::eNotMeasured, kSlowCadenceMs);
            break;
        case Stage::eHudlessRecorded:
        case Stage::eHudlessDeferred:
        case Stage::eCapturing:
            if (now - gState.stageTick > kStageTimeoutMs)
                Abandon("no Present capture (menu renderer not attached)", hd::Verdict::eUnavailable, kSlowCadenceMs);
            break;
        case Stage::eSubmitted:
            if (gState.fence && gState.fence->GetCompletedValue() >= gState.fenceValues[kFinalCaptures - 1])
                Evaluate(now);
            else if (now - gState.stageTick > kStageTimeoutMs * 4)
                Abandon("presentation capture did not complete", hd::Verdict::eUnavailable, kSlowCadenceMs);
            break;
        }
    }
    PublishClassification(now);
}

#if defined(HUDLESS_PROBE_TESTING)
void SetSourceSettleForTesting(uint64_t milliseconds) noexcept
{
    gSourceSettledMs = milliseconds;
}

void ResetForTesting() noexcept
{
    std::lock_guard lock(gState.mutex);
    Retire(gState.hudless);
    for (auto& buffer : gState.finals) Retire(buffer);
    gState.hudlessDevice.Reset();
    gState.deferredResource.Reset();
    gState.heldHudless.Reset();
    gState.captures = 0;
    gState.nextArmTick = 0;
    gState.aggregator.Reset();
    gState.last = {};
    gRelation.store(0, std::memory_order_release);
    gState.lastOffset = 0;
    gState.blockingVerdict = hd::Verdict::eNotMeasured;
    gState.reason = "none";
    gState.failures = 0;
    gState.backbufferTagged = false;
    gState.uiType = gState.uiFormat = gState.uiWidth = gState.uiHeight = 0;
    gState.uiDescribedTick = 0;
    gState.policySource = hd::Source::eWaiting;
    gState.policySourceTick = 0;
    gState.probeFrame = kNoFrame;
    gState.probePaired = gState.lastPaired = false;
    gState.lastMotionPermille = 0;
    gState.unpairedMotionProbes = 0;
    gState.motionDiffersProbes = 0;
    gPresentFrame.store(0);
    gPresentMarkers.store(0);
    gLastPresentMarkerTick.store(0);
    gLastTagFrame.store(kNoFrame);
    gTagLead.store(0);
    gMaxTagLead.store(0);
    gFirstHudlessTick.store(0);
    gLastHudlessTick.store(0);
    gHudlessEmptyTags.store(0);
    gLastUiTick.store(0);
    gFirstInputsTick.store(0);
    gDeviceIdentity.store(0);
    SetStage(Stage::eIdle);
}
#endif

std::string StatusFragment()
{
    const uint64_t now = GetTickCount64();
    std::string census;
    for (uint32_t type = 0; type < kTypeSlots; ++type)
    {
        const uint64_t value = gTypeCounts[type].load(std::memory_order_relaxed);
        if (!value) continue;
        char item[48]{};
        sprintf_s(item, "%s%u:%llu", census.empty() ? "" : ",", type,
            static_cast<unsigned long long>(value));
        census += item;
    }
    std::lock_guard lock(gState.mutex);
    const hd::Source source = CurrentSource(now);
    const hd::Verdict verdict = CurrentVerdict();
    const Api api = static_cast<Api>(gApi.load(std::memory_order_acquire));
    // Fixed fields need about 1.7 KB and the census at most 96 entries of
    // 25 characters; 8 KB keeps sprintf_s far from its abort-on-overflow path.
    static thread_local char text[8192];
    text[0] = '\0';
    sprintf_s(text, ",\"hudlessDetectEnabled\":%s,\"hudlessDetectApi\":\"%s\","
        "\"hudlessDetectSource\":\"%s\",\"hudlessDetectVerdict\":\"%s\","
        "\"hudlessDetectRoute\":\"%s\",\"hudlessDetectReason\":\"%s\","
        "\"hudlessDetectProbes\":%u,\"hudlessDetectConclusive\":%u,\"hudlessDetectFailures\":%u,"
        "\"hudlessDetectLastOutcome\":\"%s\",\"hudlessDetectIdentityPermille\":%u,"
        "\"hudlessDetectBestHypothesis\":\"%s\",\"hudlessDetectBestPermille\":%u,"
        "\"hudlessDetectUiCoveragePermille\":%u,\"hudlessDetectSignalSamples\":%u,"
        "\"hudlessDetectCompositePermille\":%u,\"hudlessDetectInformativeTiles\":%u,"
        "\"hudlessDetectTranslucentTiles\":%u,"
        "\"hudlessDetectFrameOffset\":%u,"
        "\"hudlessDetectResourceFormat\":%u,\"hudlessDetectFormat\":%u,"
        "\"hudlessDetectResourceWidth\":%u,\"hudlessDetectResourceHeight\":%u,"
        "\"hudlessDetectExtentWidth\":%u,\"hudlessDetectExtentHeight\":%u,"
        "\"hudlessDetectLifecycle\":%u,\"hudlessDetectState\":%u,"
        "\"hudlessDetectFinalFormat\":%u,\"hudlessDetectFinalWidth\":%u,"
        "\"hudlessDetectFinalHeight\":%u,\"hudlessDetectColorSpace\":%u,"
        "\"hudRecompositionState\":\"%s\",\"hudRecompositionVerified\":%s,"
        "\"hudRecompositionUiType\":%u,\"hudRecompositionUiFormat\":%u,"
        "\"hudRecompositionUiWidth\":%u,\"hudRecompositionUiHeight\":%u,"
        "\"hudlessTintEnabled\":%s,\"hudlessTintState\":\"%s\",\"hudlessTintFrames\":%llu,"
        "\"hudRecompositionUi\":\"%s\",\"hudlessSynthActive\":%s,\"hudlessSynthHealthy\":%s,"
        "\"hudlessSynthState\":\"%s\",\"hudlessSynthFrames\":%llu,\"hudlessSynthReused\":%llu,"
        "\"hudlessSynthAlpha\":\"%s\",\"hudlessSynthPrimes\":%llu,"
        "\"hudlessDetectMotionDiffersProbes\":%u,\"hudlessDetectDiffersInMotion\":%s,"
        "\"hudlessDetectPaired\":%s,\"hudlessDetectStillPermille\":%u,"
        "\"hudlessDetectUnpairedMotionProbes\":%u,\"hudlessPresentMarkers\":%llu,"
        "\"hudlessTagLead\":%d,\"hudlessMaxTagLead\":%d,"
        "\"hudlessDetectWithoutList\":%llu,\"hudlessDetectEmptyHudlessTags\":%llu,"
        "\"tagFramedCalls\":%llu,\"tagLegacyCalls\":%llu,"
        "\"tagCensus\":\"%s\"",
        gEnabled.load(std::memory_order_acquire) ? "true" : "false",
        api == Api::eD3D12 ? "d3d12" : api == Api::eVulkan ? "vulkan" : "unknown",
        hd::SourceName(source), hd::VerdictName(verdict),
        hd::RouteName(hd::Classify(source, verdict)), gState.reason,
        gState.aggregator.Probes(), gState.aggregator.Conclusive(), gState.failures,
        hd::OutcomeName(gState.last.outcome), gState.last.identityPermille,
        hd::HypothesisName(gState.last.bestHypothesis), gState.last.bestPermille,
        gState.last.uiCoveragePermille, gState.last.signalSamples,
        gState.last.compositePermille, gState.last.informativeTiles, gState.last.translucentTiles,
        gState.lastOffset,
        gState.hudlessResourceFormat, gState.hudlessFormat,
        gState.hudlessWidth, gState.hudlessHeight,
        gState.hudlessExtent.width, gState.hudlessExtent.height,
        gState.hudlessLifecycle, gState.hudlessState,
        gState.finalFormat, gState.finalWidth, gState.finalHeight, gState.colorSpace,
        RecompositionStateName(gRecompositionState.load(std::memory_order_acquire)),
        gRecompositionVerified.load(std::memory_order_acquire) ? "true" : "false",
        gState.uiType, gState.uiFormat, gState.uiWidth, gState.uiHeight,
        hudless_visualizer::Enabled() ? "true" : "false", hudless_visualizer::StateText(),
        static_cast<unsigned long long>(hudless_visualizer::FramesDrawn()),
        UiModeName(gRecompositionMode.load(std::memory_order_acquire)),
        hudless_visualizer::SynthesisActive() ? "true" : "false",
        hudless_visualizer::SynthesisHealthy() ? "true" : "false",
        hudless_visualizer::SynthesisStateText(),
        static_cast<unsigned long long>(hudless_visualizer::SynthesizedFrames()),
        static_cast<unsigned long long>(hudless_visualizer::SynthesisReusedFrames()),
        hudless_visualizer::SynthesisAlphaText(),
        static_cast<unsigned long long>(hudless_visualizer::SynthesisPrimes()),
        gState.motionDiffersProbes,
        gState.motionDiffersProbes >= 3 && verdict == hd::Verdict::eEquivalent ? "true" : "false",
        gState.lastPaired ? "true" : "false", gState.lastMotionPermille, gState.unpairedMotionProbes,
        static_cast<unsigned long long>(gPresentMarkers.load(std::memory_order_relaxed)),
        gTagLead.load(std::memory_order_relaxed), gMaxTagLead.load(std::memory_order_relaxed),
        static_cast<unsigned long long>(gHudlessWithoutList.load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(gHudlessEmptyTags.load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(gFramedCalls.load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(gLegacyCalls.load(std::memory_order_relaxed)),
        census.c_str());
    return text;
}
}
