// xemfgsa.1 (V1.7.0): XeMFG standalone on D3D12 (d3d12_host.h).
//
// Frame flow on a taken-over chain (XeFG's proxy, wrapped by the menu's proxy):
//  inputs   each source tags its depth and motion vectors on the game's own command list as they arrive, under the
//           frame's id: Streamline's tags with their lifecycle (valid only now: XeFG copies them on that list), an
//           upscaler's inputs just before the upscaler reads them (always copied). The frame's camera, jitter and
//           motion-vector scale go into its record (xess/standalone_policy.h).
//  present  the menu proxy's Present: the frame's constants, XeFG on or off for it, the frame count and its present
//           id go to XeFG, the XeLL markers the game did not send itself, then XeFG's proxy presents and generates.
//           After the present the host sleeps in XeLL for the next frame unless the game slept in Reflex.
#include "d3d12_host.h"
#include "standalone_policy.h"
#include "runtime_search.h"
#include "xemfg_dynamic.h"
#include "xemfg_window.h"
#include "sdk_pacing.h"
#include "xess_mod.h"
#include "single_module.h"
#include "optiscaler_host.h"
#include "overlay_native.h"
#include <d3d12.h>
#include <xefg_swapchain.h>
#include <xefg_swapchain_d3d12.h>
#include <xell.h>
#include <xell_d3d12.h>
#include <sl.h>
#include <nvsdk_ngx_defs.h>
#include <nvsdk_ngx_params.h>
#include <MinHook.h>
#include <psapi.h>
#include <wrl/client.h>
#include <atomic>
#include <cfloat>
#include <cstddef>
#include <filesystem>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace xess_d3d12
{
namespace
{
using Microsoft::WRL::ComPtr;
using xemfg_standalone::Source;

std::string Escape(const std::string& text)
{
    std::string out;
    for (char c : text) {
        if (c == '"' || c == '\\') out += '\\';
        if (static_cast<unsigned char>(c) >= 0x20) out += c;
    }
    return out;
}

std::string Narrow(const std::wstring& text)
{
    std::string out;
    for (wchar_t c : text) out += c < 0x80 ? char(c) : '?';
    return out;
}

void Log(const char* stage, const std::string& fields) noexcept
{
    xess_mod::WriteLog(stage, fields);
}

int64_t Now() noexcept
{
    LARGE_INTEGER value{};
    QueryPerformanceCounter(&value);
    return value.QuadPart;
}

double Milliseconds(int64_t ticks) noexcept
{
    static const double frequency = [] { LARGE_INTEGER f{}; QueryPerformanceFrequency(&f); return double(f.QuadPart); }();
    return double(ticks) * 1000.0 / frequency;
}

std::atomic<bool> gEnabled{false};
std::atomic<uint32_t> gFrames{2};
// The game's DLSS Frame Generation request (Streamline SetOptions) and FSR frame generation switch (ffxConfigure).
std::atomic<bool> gDlssgSeen{false}, gGameFrameGeneration{false}, gFsrFrameGeneration{false};
// A chain the host declined while on (the game keeps it: its DLSS-G is not held then).
std::atomic<bool> gDeclinedWhileOn{false};
// Chains created before the switch was on (restart needed).
std::atomic<bool> gNativeBeforeOn{false};
// The last depth convention an input reported (XeFG's init flag for the next chain; reverse-Z until one did).
std::atomic<bool> gObservedInverted{true};
std::atomic<uint64_t> gBasePresents{0}, gInputFrames{0};
// xemfgsa.5: the menu's FPS limit (presented frames per second; 0 off), applied as XeLL's minimum interval.
std::atomic<uint32_t> gFpsLimit{0};
thread_local bool tInsideHost = false;

// ---------------------------------------------------------------- runtime (the player's libxess_fg + libxell)
struct Runtime
{
    std::once_flag once;
    bool ready = false;
    std::string reason = "not_loaded";
    HMODULE fg = nullptr, ll = nullptr;
    std::wstring folder;
    xess_runtime_search::Result search;   // alpha.15 (B4-12): the folders searched and why the pair was not found
#define MFG_XEFG_FUNCTIONS(X) \
    X(xefgSwapChainD3D12CreateContext) X(xefgSwapChainSetLatencyReduction) X(xefgSwapChainD3D12InitFromSwapChainDesc) \
    X(xefgSwapChainD3D12GetSwapChainPtr) X(xefgSwapChainSetNumInterpolatedFrames) X(xefgSwapChainSetEnabled) \
    X(xefgSwapChainD3D12TagFrameResource) X(xefgSwapChainTagFrameConstants) X(xefgSwapChainSetPresentId) \
    X(xefgSwapChainGetLastPresentStatus) X(xefgSwapChainDestroy) X(xefgSwapChainSetLoggingCallback)
#define MFG_XELL_FUNCTIONS(X) \
    X(xellD3D12CreateContext) X(xellSetSleepMode) X(xellSleep) X(xellAddMarkerData) X(xellDestroyContext)
#define MFG_FIELD(name) decltype(&::name) name = nullptr;
    MFG_XEFG_FUNCTIONS(MFG_FIELD)
    MFG_XELL_FUNCTIONS(MFG_FIELD)
#undef MFG_FIELD
};
Runtime gRuntime;
std::atomic<bool> gRuntimeTried{false}, gRuntimeFailed{false};

// Never bundled: the pair comes from the player's install. It passes the XeFG route's discovery and instruction
// layout checks (xess_mod::AdoptVulkanHostPair, shared with the Vulkan host) before a single export is called.
Runtime& LoadRuntime()
{
    std::call_once(gRuntime.once, [] {
        Runtime& rt = gRuntime;
        xess_runtime_search::Bases bases;
        try {
            // The RTXMFG-XeSS folders first: a game's own (older) copy beside it stays the game's (runtime_search.h).
            bases = xess_runtime_search::ReadBases(xess_runtime_search::ModulePath(single_module::Self()),
                xess_runtime_search::ModulePath(nullptr));
            rt.search = xess_runtime_search::Find(bases, xess_runtime_search::Order::eD3D12);
            if (rt.search.found != SIZE_MAX) {
                const auto& folder = rt.search.folders[rt.search.found].path;
                const auto fgPath = folder / xess_runtime_search::kFg, llPath = folder / xess_runtime_search::kLl;
                rt.folder = folder.wstring();
                rt.ll = LoadLibraryExW(llPath.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
                rt.fg = rt.ll ? LoadLibraryExW(fgPath.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH) : nullptr;
            }
            if (rt.folder.empty()) rt.reason = rt.search.reason.empty() ? "runtime_not_found" : rt.search.reason;
            else if (!rt.fg || !rt.ll) rt.reason = "runtime_load_failed";
            else if (!xess_mod::AdoptVulkanHostPair(rt.fg, rt.ll)) rt.reason = "runtime_route_rejected";
            else {
                bool complete = true;
#define MFG_RESOLVE_FG(name) rt.name = reinterpret_cast<decltype(rt.name)>(GetProcAddress(rt.fg, #name)); complete &= rt.name != nullptr;
#define MFG_RESOLVE_LL(name) rt.name = reinterpret_cast<decltype(rt.name)>(GetProcAddress(rt.ll, #name)); complete &= rt.name != nullptr;
                MFG_XEFG_FUNCTIONS(MFG_RESOLVE_FG)
                MFG_XELL_FUNCTIONS(MFG_RESOLVE_LL)
#undef MFG_RESOLVE_FG
#undef MFG_RESOLVE_LL
                if (!complete) rt.reason = "runtime_exports_missing";
                else { rt.ready = true; rt.reason.clear(); }
            }
        } catch (...) {
            rt.reason = "runtime_exception";
        }
        gRuntimeTried = true;
        gRuntimeFailed = !rt.ready;
        std::string searched;
        try { searched = "," + xess_runtime_search::Json(bases, rt.search); } catch (...) {}
        Log("d3d12_runtime", std::string("\"ready\":") + (rt.ready ? "true" : "false") + ",\"folder\":\"" +
            Escape(Narrow(rt.folder)) + "\"" + (rt.reason.empty() ? "" : ",\"reason\":\"" + rt.reason + "\"") + searched);
    });
    return gRuntime;
}

void XefgLog(const char* message, xefg_swapchain_logging_level_t level, void*)
{
    static std::atomic<int> lines{0};
    if (level >= XEFG_SWAPCHAIN_LOGGING_LEVEL_WARNING && lines.fetch_add(1) < 64)
        Log("d3d12_xefg_message", "\"level\":" + std::to_string(int(level)) + ",\"text\":\"" + Escape(message ? message : "") + "\"");
}

// ---------------------------------------------------------------- chains and the host's frame state
struct Chain
{
    std::vector<IDXGISwapChain*> keys;   // XeFG's proxy as returned and its IDXGISwapChain3 (the menu proxy's
                                         // `original`): identities only, never dereferenced or owned
    xefg_swapchain_handle_t fg = nullptr;
    HWND window = nullptr;
    uint32_t width = 0, height = 0;
    bool invertedDepth = true;
    bool enabledApplied = false;
    uint32_t framesApplied = 0;
    uint64_t presents = 0;
    int64_t lastPresent = 0;
    uint32_t presentingId = 0;
    const char* lastWhy = "start";
    bool conventionLogged = false;
    bool generating = false;
    int64_t presentStart = 0;   // xemfgsa.2: when the game's Present entered XeFG's
    xefg_swapchain_present_status_t status{};
    // xemfgsa.6: Dynamic MFG on this chain: its controllers, 1X (generation off, the count kept), whether it ran last
    // frame, the game's last XeFG Present (a wait it counts) and the window's refresh rate (looked up every 2 s).
    xemfg_standalone::DynamicFrames dynamic;
    bool dynamicOff = false, dynamicRan = false;
    double lastPresentMs = 0.0;
    uint32_t refreshHz = 0;
    uint64_t refreshCheckedMs = 0, dynamicLogMs = 0;
};

constexpr size_t kRing = 8;
struct Host
{
    std::mutex mutex;
    std::vector<std::shared_ptr<Chain>> chains;
    xell_context_handle_t xell = nullptr;
    ComPtr<ID3D12Device> xellDevice;
    xemfg_standalone::FrameIds ids;
    xemfg_standalone::FrameRing<kRing> ring;
    uint32_t viewport = UINT32_MAX;     // Streamline: the first viewport that tagged depth and motion vectors
    uint32_t latestToken = 0;
    uint64_t streamlineMs = 0;          // the last Streamline depth + motion vectors
    uint64_t markersMs = 0;             // the last game latency marker
    uint64_t sleepMs = 0;               // the last game Reflex sleep that ran
    uint32_t markerPresentId = 0;       // the frame the game's present-start marker named
    // xemfgsa.2: the frame the game's render-submit start named. Not the simulation start: an engine that renders
    // a frame while it simulates the next (Unreal) has already started the next frame's simulation when the current
    // frame's upscaler runs, which gave the inputs the next frame's id.
    uint32_t markerBuildId = 0;
    uint64_t renderMarkersMs = 0;       // the last render-submit start
    Source lastSource = Source::eNone;
    std::string reason;
    // xemfgsa.2: per log interval (600 presents): the game's markers forwarded to XeLL, the game's Reflex sleeps that
    // XeLL took over, the host's own XeLL sleeps (no game markers), and the time in XeFG's Present and in XeLL sleeps.
    uint64_t markers = 0, reflexSleeps = 0, ownSleeps = 0;
    int64_t presentTicks = 0, sleepTicks = 0;
    uint32_t intervalPresents = 0;
    // xemfgsa.5: the game's and XeFG's frame rates for the menu, and the XeLL interval the FPS limit last set.
    xemfg_standalone::FpsWindow fps;
    uint32_t limitIntervalUs = 0;
    // xemfgsa.5 (log): Streamline tag/constant calls whose token mapped to a frame already presented (Subnautica 2,
    // xemfgsa.4: the tags were seen but never reached XeFG; the DLSS evaluation's inputs ran the whole session), with
    // the raw tokens of the game's latest render-submit and present markers to compare.
    uint64_t streamlineRefused = 0, streamlineRefusedLogs = 0;
    uint32_t renderToken = 0, presentToken = 0;
    // xemfgsa.6: the menu's Dynamic MFG settings (the most frames and the FPS limit are filled in per frame), and the
    // game's XeLL sleep since its last present (Dynamic MFG counts it as waiting).
    xemfg_standalone::DynamicSettings dynamic;
    int64_t frameSleepTicks = 0;
};
Host gHost;

uint64_t NowMs() noexcept { return GetTickCount64(); }

std::shared_ptr<Chain> FindLocked(IDXGISwapChain* key)
{
    if (!key) return nullptr;
    for (const auto& chain : gHost.chains)
        for (IDXGISwapChain* known : chain->keys) if (known == key) return chain;
    return nullptr;
}

// The chain inputs go to: the newest one (one game window).
std::shared_ptr<Chain> ActiveLocked()
{
    return gHost.chains.empty() ? nullptr : gHost.chains.back();
}

void NoteReason(const std::string& reason)
{
    std::lock_guard lock(gHost.mutex);
    gHost.reason = reason;
}

bool MarkersRecent(uint64_t now) noexcept { return gHost.markersMs && now - gHost.markersMs < 1000; }
bool GameSleeps(uint64_t now) noexcept { return gHost.sleepMs && now - gHost.sleepMs < 1000; }

// Whether frames should carry inputs at all (copies cost GPU time): the switch, no FSR FG, the game's DLSS FG on
// when it drives one.
bool Wanted() noexcept
{
    return gEnabled.load() && !gFsrFrameGeneration.load() && (!gDlssgSeen.load() || gGameFrameGeneration.load());
}

// The frame an upscaler evaluation belongs to: the frame the game's latest render-submit start named, else the next.
uint32_t BuildIdLocked(uint64_t now)
{
    const bool renderMarkers = gHost.renderMarkersMs && now - gHost.renderMarkersMs < 1000;
    if (renderMarkers && gHost.markerBuildId > gHost.ids.Last()) return gHost.markerBuildId;
    if (gHost.ids.Tokens() && !MarkersRecent(now) &&
        (!gHost.streamlineMs || now - gHost.streamlineMs >= xemfg_standalone::kStreamlineHoldMs))
        gHost.ids.LeaveTokens();
    return gHost.ids.Next();
}

ComPtr<ID3D12GraphicsCommandList> NativeList(void* list)
{
    ComPtr<ID3D12GraphicsCommandList> native;
    if (list && !single_overlay::native::Unwrap(static_cast<IUnknown*>(list), native)) native.Reset();
    return native;
}

bool Tag(const Chain& chain, ID3D12GraphicsCommandList* list, uint32_t id, xefg_swapchain_resource_type_t type,
    ID3D12Resource* resource, const xemfg_standalone::Area& area, uint32_t state, bool onlyNow)
{
    if (!resource || !area.width || !area.height || (onlyNow && !list)) return false;
    xefg_swapchain_d3d12_resource_data_t data{};
    data.type = type;
    data.validity = onlyNow ? XEFG_SWAPCHAIN_RV_ONLY_NOW : XEFG_SWAPCHAIN_RV_UNTIL_NEXT_PRESENT;
    data.resourceBase = {area.x, area.y};
    data.resourceSize = {area.width, area.height};
    data.pResource = resource;
    data.incomingState = static_cast<D3D12_RESOURCE_STATES>(state);
    xess_mod::VulkanHostCallScope scope;
    return gRuntime.xefgSwapChainD3D12TagFrameResource(chain.fg, onlyNow ? list : nullptr, id, &data) == XEFG_SWAPCHAIN_RESULT_SUCCESS;
}

void LogSource(Source source)
{
    if (source == gHost.lastSource) return;
    gHost.lastSource = source;
    Log("d3d12_input_source", std::string("\"source\":\"") + xemfg_standalone::SourceName(source) + "\"");
}

// An upscaler's depth and motion vectors for the frame being built, tagged before the upscaler reads them.
struct UpscalerFrame
{
    Source source = Source::eNone;
    ID3D12GraphicsCommandList* list = nullptr;
    ID3D12Resource* depth = nullptr;
    ID3D12Resource* motion = nullptr;
    uint32_t depthState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    uint32_t motionState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    xemfg_standalone::Area depthArea, motionArea;
    float jitterX = 0, jitterY = 0, motionScaleX = 1, motionScaleY = 1;
    bool reset = false;
    bool invertedKnown = false, inverted = false;
    float cameraNear = 0, cameraFar = 0, cameraVerticalFov = 0, frameTimeMs = 0;
};

void TagUpscalerFrame(const UpscalerFrame& in)
{
    if (!in.list || !in.depth || !in.motion || !Wanted()) return;
    const uint64_t now = NowMs();
    std::lock_guard lock(gHost.mutex);
    if (!xemfg_standalone::SourceAdmitted(in.source, now, gHost.streamlineMs)) return;
    auto chain = ActiveLocked();
    if (!chain || !chain->fg) return;
    const uint32_t id = BuildIdLocked(now);
    auto& record = gHost.ring.At(id);
    if (record.depth && record.motion) return;   // one evaluation per frame
    if (!xemfg_standalone::Claim(record, in.source)) return;
    auto list = NativeList(in.list);
    if (!list) return;
    record.depthWidth = in.depthArea.width;
    record.depthHeight = in.depthArea.height;
    record.motionWidth = in.motionArea.width;
    record.motionHeight = in.motionArea.height;
    // XeFG reads depth and motion vectors at one size: display-size vectors are not tagged (this frame shows why).
    if (record.depthWidth != record.motionWidth || record.depthHeight != record.motionHeight) {
        record.depth = record.motion = true;
        return;
    }
    record.depth = Tag(*chain, list.Get(), id, XEFG_SWAPCHAIN_RES_DEPTH, in.depth, in.depthArea, in.depthState, true);
    record.motion = Tag(*chain, list.Get(), id, XEFG_SWAPCHAIN_RES_MOTION_VECTOR, in.motion, in.motionArea, in.motionState, true);
    record.constants = true;
    record.jitterX = in.jitterX;
    record.jitterY = in.jitterY;
    record.motionScaleX = in.motionScaleX;
    record.motionScaleY = in.motionScaleY;
    record.reset = in.reset;
    record.depthInvertedKnown = in.invertedKnown;
    record.depthInverted = in.inverted;
    record.cameraNear = in.cameraNear;
    record.cameraFar = in.cameraFar;
    record.cameraVerticalFov = in.cameraVerticalFov;
    record.frameTimeMs = in.frameTimeMs;
    if (in.invertedKnown) gObservedInverted = in.inverted;
    if (record.depth && record.motion) { ++gInputFrames; LogSource(in.source); }
}

// ---------------------------------------------------------------- the XeLL context of the game's device
bool EnsureXell(ID3D12Device* device, std::string& reason)
{
    if (gHost.xell && gHost.xellDevice.Get() == device) return true;
    Runtime& rt = gRuntime;
    if (gHost.xell) {
        if (!gHost.chains.empty()) { reason = "second_device"; return false; }
        xess_mod::VulkanHostCallScope scope;
        rt.xellDestroyContext(gHost.xell);
        gHost.xell = nullptr;
        gHost.xellDevice.Reset();
    }
    xess_mod::VulkanHostCallScope scope;
    xell_context_handle_t xell = nullptr;
    if (rt.xellD3D12CreateContext(device, &xell) != XELL_RESULT_SUCCESS || !xell) { reason = "xell_create"; return false; }
    // XeFG generates frames only with XeLL's low-latency mode on (vk_host.cpp EnsureLayer).
    xell_sleep_params_t sleep{};
    sleep.bLowLatencyMode = 1;
    if (rt.xellSetSleepMode(xell, &sleep) != XELL_RESULT_SUCCESS) {
        rt.xellDestroyContext(xell);
        reason = "xell_sleep_mode";
        return false;
    }
    gHost.xell = xell;
    gHost.xellDevice = device;
    gHost.limitIntervalUs = 0;
    return true;
}

void Marker(uint32_t id, xell_latency_marker_type_t marker)
{
    if (!gHost.xell || !id) return;
    xess_mod::VulkanHostCallScope scope;
    gRuntime.xellAddMarkerData(gHost.xell, id, marker);
}

// xemfgsa.5: before each XeLL sleep (host lock held). The game's Reflex sleep goes to XeLL while XeMFG generates
// (ReplacesReflexSleep), so the driver's Reflex frame limit no longer paces it: the menu's FPS limit becomes XeLL's
// minimum interval, which XeLL holds for the frames XeFG presents (standalone_policy.h LimitIntervalUs).
void ApplyFpsLimitLocked()
{
    if (!gHost.xell) return;
    const auto chain = ActiveLocked();
    const uint32_t perBase = chain && chain->generating ? chain->framesApplied : 1;
    // xemfgsa.6: Dynamic MFG's "Use target as cap" holds the presented rate at its target instead.
    const double capFps = chain ? chain->dynamic.CapFps() : 0.0;
    const uint32_t fps = capFps > 0.0 ? uint32_t(capFps + 0.5) : gFpsLimit.load();
    const uint32_t interval = xemfg_standalone::LimitIntervalUs(fps);
    if (interval == gHost.limitIntervalUs) return;
    xell_sleep_params_t sleep{};
    sleep.bLowLatencyMode = 1;
    sleep.minimumIntervalUs = interval;
    xess_mod::VulkanHostCallScope scope;
    const auto result = gRuntime.xellSetSleepMode(gHost.xell, &sleep);
    if (result == XELL_RESULT_SUCCESS) gHost.limitIntervalUs = interval;
    Log("d3d12_fps_limit", "\"fps\":" + std::to_string(fps) + ",\"per_base\":" + std::to_string(perBase) +
        ",\"interval_us\":" + std::to_string(interval) + ",\"result\":" + std::to_string(int(result)));
}

// ---------------------------------------------------------------- takeover policy
bool SupportedFormat(DXGI_FORMAT format) noexcept
{
    switch (format) {
    case DXGI_FORMAT_R8G8B8A8_UNORM: case DXGI_FORMAT_B8G8R8A8_UNORM: case DXGI_FORMAT_R10G10B10A2_UNORM:
    case DXGI_FORMAT_R16G16B16A16_FLOAT:
        return true;
    default:
        return false;
    }
}

// The game's own XeSS FG creates its chain from inside libxess_fg: that chain stays the game's.
bool FromFrameGenerator(void* caller) noexcept
{
    HMODULE owner = nullptr;
    if (!caller || !GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                      static_cast<LPCWSTR>(caller), &owner) || !owner)
        return false;
    return GetProcAddress(owner, "xefgSwapChainD3D12InitFromSwapChainDesc") != nullptr;
}

const char* Refusal(HWND window, const DXGI_SWAP_CHAIN_DESC1& desc, void* caller)
{
    if (optiscaler_host::Detected()) return "optiscaler";   // OptiScaler hosts XeFG itself
    if (!xess_mod::Self()) return "xess_route_not_started";
    if (tInsideHost) return "nested";
    if (FromFrameGenerator(caller)) return "game_xefg";
    if (!window || !IsWindow(window)) return "window";
    if (desc.SwapEffect != DXGI_SWAP_EFFECT_FLIP_DISCARD && desc.SwapEffect != DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL) return "swap_effect";
    if (desc.SampleDesc.Count != 1 || desc.Stereo) return "swapchain_flags";
    if (!SupportedFormat(desc.Format)) return "swapchain_format";
    if (desc.BufferCount < 2 || desc.BufferCount > 8) return "buffer_count";
    return nullptr;
}
} // namespace

// ---------------------------------------------------------------- configuration
void Configure(bool enabled, uint32_t frames) noexcept
{
    const bool was = gEnabled.exchange(enabled);
    const uint32_t previous = gFrames.exchange(std::clamp<uint32_t>(frames ? frames : 2, 2, 6));
    if (was != enabled || previous != gFrames.load())
        Log("d3d12_config", std::string("\"enabled\":") + (enabled ? "true" : "false") + ",\"frames\":" + std::to_string(gFrames.load()));
}

bool Enabled() noexcept { return gEnabled.load(); }

void SetFpsLimit(uint32_t fps) noexcept
{
    if (gFpsLimit.exchange(fps) != fps) Log("d3d12_fps_limit_config", "\"fps\":" + std::to_string(fps));
}

void SetDynamic(bool enabled, uint32_t targetFps, int rounding, bool allow1x, bool targetAsCap) noexcept
{
    try {
        std::lock_guard lock(gHost.mutex);
        auto next = gHost.dynamic;
        next.enabled = enabled;
        next.targetFps = std::min<uint32_t>(targetFps, 1000);
        next.rounding = std::clamp(rounding, 0, 2);
        next.allow1x = allow1x;
        next.targetAsCap = targetAsCap;
        if (next == gHost.dynamic) return;
        gHost.dynamic = next;
        Log("d3d12_dynamic_config", std::string("\"enabled\":") + (enabled ? "true" : "false") + ",\"target\":" +
            std::to_string(next.targetFps) + ",\"rounding\":" + std::to_string(next.rounding) + ",\"allow_1x\":" +
            (allow1x ? "true" : "false") + ",\"target_as_cap\":" + (targetAsCap ? "true" : "false"));
    } catch (...) {}
}

bool InsideTakeover() noexcept { return tInsideHost; }

bool ReplacesFrameGeneration() noexcept
{
    if (!gEnabled.load() || gRuntimeFailed.load() || optiscaler_host::Detected()) return false;
    std::lock_guard lock(gHost.mutex);
    return !gHost.chains.empty() || (!gDeclinedWhileOn.load() && !gNativeBeforeOn.load());
}

void GameFrameGeneration(bool on) noexcept
{
    gDlssgSeen = true;
    if (gGameFrameGeneration.exchange(on) != on)
        Log("d3d12_game_frame_generation", std::string("\"on\":") + (on ? "true" : "false"));
}

// ---------------------------------------------------------------- swap chains
bool CreateSwapChain(IUnknown* queue, HWND window, const DXGI_SWAP_CHAIN_DESC1* desc,
    const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fullscreen, IDXGIOutput*, void* caller, IDXGISwapChain1** output,
    HRESULT& hr) noexcept
{
    if (!output || !desc || !queue) return false;
    ComPtr<ID3D12CommandQueue> native;
    if (!single_overlay::native::Unwrap(queue, native) || !native) {   // not a D3D12 chain
        // alpha.15 (B4-26): said once while XeMFG standalone is on (Metro Redux's DirectX 11 chain was silent).
        static std::atomic<bool> logged{false};
        if (gEnabled.load() && !logged.exchange(true))
            Log("d3d12_takeover_refused", "\"reason\":\"not_d3d12\",\"width\":" + std::to_string(desc->Width) +
                ",\"height\":" + std::to_string(desc->Height) + ",\"format\":" + std::to_string(int(desc->Format)));
        return false;
    }
    if (!gEnabled.load()) {
        gNativeBeforeOn = true;
        return false;
    }
    const char* refused = Refusal(window, *desc, caller);
    if (refused) {
        if (strcmp(refused, "nested") && strcmp(refused, "game_xefg")) gDeclinedWhileOn = true;
        NoteReason(refused);
        Log("d3d12_takeover_refused", std::string("\"reason\":\"") + refused + "\",\"width\":" + std::to_string(desc->Width) +
            ",\"height\":" + std::to_string(desc->Height) + ",\"format\":" + std::to_string(int(desc->Format)));
        return false;
    }
    std::string reason;
    auto chain = std::make_shared<Chain>();
    try {
        tInsideHost = true;
        struct Leave { ~Leave() { tInsideHost = false; } } leave;
        const D3D12_COMMAND_QUEUE_DESC queueDesc = native->GetDesc();
        ComPtr<ID3D12Device> device;
        ComPtr<IDXGIFactory2> factory;
        Runtime& rt = LoadRuntime();
        if (queueDesc.Type != D3D12_COMMAND_LIST_TYPE_DIRECT) reason = "queue_type";
        else if (FAILED(native->GetDevice(IID_PPV_ARGS(&device))) || !device) reason = "device";
        else if (!rt.ready) reason = rt.reason;
        else {
            // XeFG creates the real chain on a system factory: a factory proxy above us (Streamline's) sees the
            // game's chain only, and one present per base frame.
            using CreateFactory2 = HRESULT(WINAPI*)(UINT, REFIID, void**);
            const HMODULE dxgi = single_module::LoadSystemModule(L"dxgi.dll");
            auto create = dxgi ? reinterpret_cast<CreateFactory2>(GetProcAddress(dxgi, "CreateDXGIFactory2")) : nullptr;
            if (!create || FAILED(create(0, IID_PPV_ARGS(&factory))) || !factory) reason = "system_factory";
        }
        if (reason.empty()) {
            std::lock_guard lock(gHost.mutex);
            if (EnsureXell(device.Get(), reason)) {
                // xemfgsa.3: no flip queue and the game's buffer count on this device (xess_mod ConfigureFlipQueue).
                xess_mod::SetStandaloneHostDevice(device.Get());
                xess_mod::VulkanHostCallScope scope;
                if (rt.xefgSwapChainD3D12CreateContext(device.Get(), &chain->fg) != XEFG_SWAPCHAIN_RESULT_SUCCESS || !chain->fg) {
                    chain->fg = nullptr;
                    reason = "xefg_create";
                } else {
                    rt.xefgSwapChainSetLoggingCallback(chain->fg, XEFG_SWAPCHAIN_LOGGING_LEVEL_WARNING, XefgLog, nullptr);
                    if (rt.xefgSwapChainSetLatencyReduction(chain->fg, gHost.xell) != XEFG_SWAPCHAIN_RESULT_SUCCESS) reason = "xefg_latency";
                }
            }
        }
        ComPtr<IDXGISwapChain1> proxy;
        if (reason.empty()) {
            // The game's depth convention is known only once its inputs arrive: reverse-Z until an input reported
            // one (most engines), the observed one for later chains.
            chain->invertedDepth = gObservedInverted.load();
            xefg_swapchain_d3d12_init_params_t params{};
            params.initFlags = chain->invertedDepth ? XEFG_SWAPCHAIN_INIT_FLAG_INVERTED_DEPTH : 0;
            params.maxInterpolatedFrames = 5;
            params.uiMode = XEFG_SWAPCHAIN_UI_MODE_AUTO;
            xess_mod::VulkanHostCallScope scope;
            auto result = rt.xefgSwapChainD3D12InitFromSwapChainDesc(chain->fg, window, desc, fullscreen, native.Get(), factory.Get(), &params);
            if (result != XEFG_SWAPCHAIN_RESULT_SUCCESS) {
                // Without the 5-frame allocation (an unlock the route refused leaves XeFG's stock limit).
                params.maxInterpolatedFrames = XEFG_SWAPCHAIN_USE_MAX_SUPPORTED_INTERPOLATED_FRAMES;
                result = rt.xefgSwapChainD3D12InitFromSwapChainDesc(chain->fg, window, desc, fullscreen, native.Get(), factory.Get(), &params);
            }
            const char* menu = nullptr;
            if (result != XEFG_SWAPCHAIN_RESULT_SUCCESS) reason = "xefg_init_" + std::to_string(int(result));
            // alpha.15 (B4-05): the game's chain gets no menu here, so a takeover whose output chain did not get it
            // either would leave no menu and no menu key (and the saved switch repeats it every launch). Fail it:
            // the game keeps its native chain with the menu, which shows this reason.
            else if (single_module::OverlayAllowed() && !xess_mod::HostOutputAttached(chain->fg, &menu))
                reason = std::string("menu_") + (menu ? menu : "not_created");
            else if (rt.xefgSwapChainD3D12GetSwapChainPtr(chain->fg, IID_PPV_ARGS(&proxy)) != XEFG_SWAPCHAIN_RESULT_SUCCESS || !proxy)
                reason = "xefg_proxy";
            else {
                const uint32_t frames = gFrames.load();
                rt.xefgSwapChainSetNumInterpolatedFrames(chain->fg, frames - 1);
                chain->framesApplied = frames;
                rt.xefgSwapChainSetEnabled(chain->fg, 0);
            }
        }
        if (!reason.empty()) {
            proxy.Reset();
            if (chain->fg) { xess_mod::VulkanHostCallScope scope; rt.xefgSwapChainDestroy(chain->fg); }
            gDeclinedWhileOn = true;
            NoteReason(reason);
            Log("d3d12_takeover_failed", "\"reason\":\"" + Escape(reason) + "\"");
            return false;
        }
        chain->window = window;
        chain->width = desc->Width;
        chain->height = desc->Height;
        if ((!chain->width || !chain->height) && window) {
            RECT client{};
            if (GetClientRect(window, &client)) { chain->width = uint32_t(client.right); chain->height = uint32_t(client.bottom); }
        }
        chain->lastPresent = Now();
        chain->keys.push_back(proxy.Get());
        ComPtr<IDXGISwapChain3> version3;
        if (SUCCEEDED(proxy.As(&version3)) && version3.Get() != static_cast<IDXGISwapChain*>(proxy.Get()))
            chain->keys.push_back(version3.Get());
        {
            std::lock_guard lock(gHost.mutex);
            gHost.chains.push_back(chain);
            gHost.ring.Clear();
            gHost.reason.clear();
        }
        Log("d3d12_takeover", "\"width\":" + std::to_string(chain->width) + ",\"height\":" + std::to_string(chain->height) +
            ",\"format\":" + std::to_string(int(desc->Format)) + ",\"buffers\":" + std::to_string(desc->BufferCount) +
            ",\"flags\":" + std::to_string(desc->Flags) + ",\"inverted_depth\":" + (chain->invertedDepth ? "true" : "false") +
            ",\"frames\":" + std::to_string(chain->framesApplied) + ",\"runtime\":\"" + Escape(Narrow(gRuntime.folder)) + "\"");
        *output = proxy.Detach();
        hr = S_OK;
        return true;
    } catch (...) {
        if (chain->fg) { xess_mod::VulkanHostCallScope scope; gRuntime.xefgSwapChainDestroy(chain->fg); }
        NoteReason("takeover_exception");
        return false;
    }
}

namespace
{
std::shared_ptr<Chain> Remove(IDXGISwapChain* key)
{
    std::lock_guard lock(gHost.mutex);
    auto chain = FindLocked(key);
    if (chain) gHost.chains.erase(std::find(gHost.chains.begin(), gHost.chains.end(), chain));
    return chain;
}

void Destroy(const std::shared_ptr<Chain>& chain, const char* why)
{
    if (!chain || !chain->fg) return;
    xefg_swapchain_result_t result;
    {
        xess_mod::VulkanHostCallScope scope;
        result = gRuntime.xefgSwapChainDestroy(chain->fg);
    }
    chain->fg = nullptr;
    Log("d3d12_released", std::string("\"why\":\"") + why + "\",\"result\":" + std::to_string(int(result)) +
        ",\"presents\":" + std::to_string(chain->presents));
}
}

void Abandon(IDXGISwapChain* chain) noexcept
{
    try {
        auto record = Remove(chain);
        if (!record) return;
        chain->Release();   // the reference CreateSwapChain returned
        Destroy(record, "menu_proxy_refused");
        gDeclinedWhileOn = true;
        NoteReason("menu_proxy_refused");
    } catch (...) {}
}

bool Owns(IDXGISwapChain* chain) noexcept
{
    std::lock_guard lock(gHost.mutex);
    return FindLocked(chain) != nullptr;
}

void ChainReleased(IDXGISwapChain* chain) noexcept
{
    try { Destroy(Remove(chain), "game_release"); } catch (...) {}
}

void BeforePresent(IDXGISwapChain* key) noexcept
{
    try {
        const uint64_t now = NowMs();
        std::lock_guard lock(gHost.mutex);
        auto chain = FindLocked(key);
        if (!chain || !chain->fg) return;
        Runtime& rt = gRuntime;
        uint32_t pending[kRing]{};
        const size_t count = gHost.ring.Pending(gHost.ids.Last(), pending, kRing);
        const uint32_t id = xemfg_standalone::PresentId(gHost.ids.Last(), gHost.markerPresentId, pending, count);
        gHost.markerPresentId = 0;
        const int64_t ticks = Now();
        const float frameMs = float(Milliseconds(ticks - chain->lastPresent));
        chain->lastPresent = ticks;
        // The output aspect after the game's ResizeBuffers (XeFG's proxy resizes itself).
        if ((chain->presents % 120) == 0) {
            DXGI_SWAP_CHAIN_DESC desc{};
            if (SUCCEEDED(key->GetDesc(&desc)) && desc.BufferDesc.Width && desc.BufferDesc.Height) {
                chain->width = desc.BufferDesc.Width;
                chain->height = desc.BufferDesc.Height;
            }
        }
        const auto* record = gHost.ring.Find(id);
        xemfg_standalone::GenerationInputs inputs;
        inputs.enabled = gEnabled.load();
        inputs.depth = record && record->depth;
        inputs.motion = record && record->motion;
        inputs.sizesMatch = !record || (record->depthWidth == record->motionWidth && record->depthHeight == record->motionHeight);
        inputs.dlssgSeen = gDlssgSeen.load();
        inputs.gameFrameGeneration = gGameFrameGeneration.load();
        inputs.fsrFrameGeneration = gFsrFrameGeneration.load();
        const char* why = xemfg_standalone::WhyNotGenerating(inputs);
        bool tagged = false;
        xess_mod::VulkanHostCallScope scope;
        if (!why) {
            const bool inverted = record->depthInvertedKnown ? record->depthInverted : chain->invertedDepth;
            if (record->depthInvertedKnown && record->depthInverted != chain->invertedDepth && !chain->conventionLogged) {
                chain->conventionLogged = true;
                Log("d3d12_depth_convention", std::string("\"inputs_inverted\":") + (record->depthInverted ? "true" : "false") +
                    ",\"chain_inverted\":" + (chain->invertedDepth ? "true" : "false") + ",\"applies\":\"next_chain\"");
            }
            xefg_swapchain_frame_constant_data_t constants{};
            const float identity[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
            memcpy(constants.viewMatrix, identity, sizeof(identity));
            const float aspect = chain->height ? float(chain->width) / float(chain->height) : 1.0f;
            xemfg_standalone::Projection(aspect, record->cameraNear, record->cameraFar, record->cameraVerticalFov, inverted,
                                         constants.projectionMatrix);
            constants.jitterOffsetX = record->jitterX;
            constants.jitterOffsetY = record->jitterY;
            auto scale = std::make_pair(record->motionScaleX, record->motionScaleY);
            if (record->motionNormalized)
                scale = xemfg_standalone::StreamlineMotionScale(record->motionScaleX, record->motionScaleY, record->motionWidth,
                                                                record->motionHeight);
            constants.motionVectorScaleX = scale.first;
            constants.motionVectorScaleY = scale.second;
            constants.resetHistory = record->reset ? 1 : 0;
            constants.frameRenderTime = record->frameTimeMs > 0 ? record->frameTimeMs : frameMs;
            tagged = rt.xefgSwapChainTagFrameConstants(chain->fg, id, &constants) == XEFG_SWAPCHAIN_RESULT_SUCCESS;
            if (!tagged) why = "constants";
        }
        // xemfgsa.6: Dynamic MFG picks this frame's count, up to the frames set (1 = generation off, 1X: XeFG keeps its
        // count and comes back on in microseconds). It runs while XeFG could generate; when generation comes back
        // (the game's switch, inputs) its averages start over and what it learned stays.
        uint32_t frames = gFrames.load();
        bool dynamicOff = false;
        const double sleepMs = Milliseconds(gHost.frameSleepTicks);
        gHost.frameSleepTicks = 0;
        if (!why && gHost.dynamic.enabled) {
            if (!chain->dynamicRan) chain->dynamic.Restart();
            if (!chain->refreshCheckedMs || now - chain->refreshCheckedMs >= 2000) {
                chain->refreshCheckedMs = now;
                chain->refreshHz = xemfg_standalone::WindowRefreshHz(chain->window);
            }
            xemfg_standalone::DynamicFrames::Frame frame;
            frame.nowMs = Milliseconds(ticks);
            frame.frameMs = frameMs;
            frame.sleepMs = sleepMs;
            frame.presentMs = chain->lastPresentMs;
            frame.refreshFps = double(chain->refreshHz);
            frame.focus = xemfg_standalone::WindowFocus(chain->window);
            frame.frames = chain->dynamicOff ? 1 : chain->framesApplied ? chain->framesApplied : frames;
            auto settings = gHost.dynamic;
            settings.maxFrames = frames;
            settings.limitFps = gFpsLimit.load();
            const uint32_t wanted = chain->dynamic.Update(frame, settings);
            if (chain->dynamic.Changed() || now - chain->dynamicLogMs >= 5000) {
                chain->dynamicLogMs = now;
                Log("xemfg_dynamic", "\"api\":\"d3d12\"," + chain->dynamic.Describe(frame.frames, wanted));
            }
            dynamicOff = wanted <= 1;
            if (!dynamicOff) frames = wanted;
            chain->dynamicRan = true;
        } else {
            if (!gHost.dynamic.enabled && chain->dynamic.Active()) chain->dynamic.Reset();
            chain->dynamicRan = false;
        }
        const bool enabled = !why && !dynamicOff;
        if (enabled != chain->enabledApplied) {
            rt.xefgSwapChainSetEnabled(chain->fg, enabled ? 1 : 0);
            chain->enabledApplied = enabled;
        }
        if (!dynamicOff && frames != chain->framesApplied &&
            rt.xefgSwapChainSetNumInterpolatedFrames(chain->fg, frames - 1) == XEFG_SWAPCHAIN_RESULT_SUCCESS)
            chain->framesApplied = frames;
        chain->dynamicOff = dynamicOff;
        rt.xefgSwapChainSetPresentId(chain->fg, id);
        if (!MarkersRecent(now)) {
            Marker(id, XELL_SIMULATION_END);
            Marker(id, XELL_RENDERSUBMIT_START);
            Marker(id, XELL_RENDERSUBMIT_END);
            Marker(id, XELL_PRESENT_START);
        }
        chain->presentingId = id;
        if (why != chain->lastWhy && (!why || !chain->lastWhy || strcmp(why, chain->lastWhy))) {
            Log("d3d12_generation", std::string("\"frame\":") + std::to_string(id) + ",\"generating\":" + (why ? "false" : "true") +
                (why ? std::string(",\"why\":\"") + why + "\"" : std::string()) + ",\"source\":\"" +
                xemfg_standalone::SourceName(record ? record->source : Source::eNone) + "\"");
            chain->lastWhy = why;
        }
        chain->generating = enabled;
        gHost.reason = why ? why : "";
        chain->presentStart = Now();
    } catch (...) {}
}

void AfterPresent(IDXGISwapChain* key, HRESULT result) noexcept
{
    try {
        uint32_t next = 0;
        bool sleep = false;
        xell_context_handle_t xell = nullptr;
        {
            const uint64_t now = NowMs();
            std::lock_guard lock(gHost.mutex);
            auto chain = FindLocked(key);
            if (!chain || !chain->fg || !chain->presentingId) return;
            const uint32_t id = chain->presentingId;
            chain->presentingId = 0;
            const bool markers = MarkersRecent(now);
            if (!markers) Marker(id, XELL_PRESENT_END);
            gHost.ids.Presented(id);
            ++chain->presents;
            ++gBasePresents;
            gHost.fps.Add(now, gBasePresents.load(), sdk_pacing::OutputPresents());   // xemfgsa.5
            if (chain->presentStart) {
                gHost.presentTicks += Now() - chain->presentStart;
                chain->lastPresentMs = Milliseconds(Now() - chain->presentStart);   // xemfgsa.6: Dynamic MFG's wait
            }
            chain->presentStart = 0;
            ++gHost.intervalPresents;
            if ((chain->presents & 63) == 1) {
                xess_mod::VulkanHostCallScope scope;
                gRuntime.xefgSwapChainGetLastPresentStatus(chain->fg, &chain->status);
            }
            if (chain->presents == 1 || (chain->presents % 600) == 0) {
                // xemfgsa.2: where the frame's time goes, per base frame over the interval.
                const double frames = double(std::max<uint32_t>(gHost.intervalPresents, 1));
                char timing[160]{};
                snprintf(timing, sizeof(timing), ",\"present_ms\":%.3f,\"xell_sleep_ms\":%.3f", Milliseconds(gHost.presentTicks) / frames,
                         Milliseconds(gHost.sleepTicks) / frames);
                Log("d3d12_present", "\"presents\":" + std::to_string(chain->presents) + ",\"generating\":" +
                    (chain->generating ? "true" : "false") + ",\"frames_presented\":" + std::to_string(chain->status.framesPresented) +
                    ",\"fg_result\":" + std::to_string(int(chain->status.frameGenResult)) + ",\"hresult\":" + std::to_string(result) +
                    ",\"source\":\"" + xemfg_standalone::SourceName(gHost.lastSource) + "\"" + timing +
                    ",\"game_markers\":" + std::to_string(gHost.markers) + ",\"reflex_sleeps_to_xell\":" + std::to_string(gHost.reflexSleeps) +
                    ",\"own_xell_sleeps\":" + std::to_string(gHost.ownSleeps) + ",\"interval_presents\":" + std::to_string(gHost.intervalPresents) +
                    ",\"game_thread\":" + std::to_string(GetCurrentThreadId()) +   // xemfgsa.4: names a fault's thread
                    ",\"streamline_refused\":" + std::to_string(gHost.streamlineRefused));   // xemfgsa.5
                gHost.markers = gHost.reflexSleeps = gHost.ownSleeps = 0;
                gHost.streamlineRefused = 0;
                gHost.presentTicks = gHost.sleepTicks = 0;
                gHost.intervalPresents = 0;
            }
            next = id + 1;
            // The next frame starts here unless the game names its frames itself.
            if (!markers) {
                sleep = !GameSleeps(now);
                xell = gHost.xell;
                if (sleep) ApplyFpsLimitLocked();
            }
        }
        // Never under the host's lock: XeLL's sleep waits for the GPU and other threads keep tagging.
        if (xell && sleep) {
            const int64_t start = Now();
            {
                xess_mod::VulkanHostCallScope scope;
                gRuntime.xellSleep(xell, next);
            }
            std::lock_guard lock(gHost.mutex);
            gHost.sleepTicks += Now() - start;
            gHost.frameSleepTicks += Now() - start;   // xemfgsa.6
            ++gHost.ownSleeps;
        }
        if (xell) { std::lock_guard lock(gHost.mutex); Marker(next, XELL_SIMULATION_START); }
    } catch (...) {}
}

// ---------------------------------------------------------------- Streamline inputs
namespace
{
// xemfgsa.5 (log): a Streamline call whose token mapped to no frame (host lock held).
void NoteStreamlineRefusedLocked(const char* call, uint32_t token)
{
    ++gHost.streamlineRefused;
    if (gHost.streamlineRefusedLogs++ >= 8) return;
    Log("d3d12_streamline_refused", std::string("\"call\":\"") + call + "\",\"token\":" + std::to_string(token) +
        ",\"offset\":" + std::to_string(gHost.ids.Offset()) + ",\"last_presented\":" + std::to_string(gHost.ids.Last()) +
        ",\"render_token\":" + std::to_string(gHost.renderToken) + ",\"present_token\":" + std::to_string(gHost.presentToken) +
        ",\"latest_token\":" + std::to_string(gHost.latestToken));
}
}

void StreamlineTags(uint32_t viewport, bool framed, uint32_t frame, const sl::ResourceTag* tags, uint32_t count,
    void* commandList) noexcept
{
    if (!tags || !count || count > 1024 || !Wanted()) return;
    try {
        std::lock_guard lock(gHost.mutex);
        auto chain = ActiveLocked();
        if (!chain || !chain->fg) return;
        if (gHost.viewport != UINT32_MAX && gHost.viewport != viewport) return;
        const uint32_t token = framed ? frame : gHost.latestToken;
        if (!token && !framed) return;
        if (framed && frame > gHost.latestToken) gHost.latestToken = frame;
        const uint32_t id = gHost.ids.FromToken(token);
        if (!id) { NoteStreamlineRefusedLocked("tags", token); return; }
        auto& record = gHost.ring.At(id);
        if (!xemfg_standalone::Claim(record, Source::eStreamline)) return;
        auto list = NativeList(commandList);
        for (uint32_t i = 0; i < count; ++i) {
            const auto& tag = tags[i];
            xefg_swapchain_resource_type_t type;
            if (tag.type == sl::kBufferTypeDepth) type = XEFG_SWAPCHAIN_RES_DEPTH;
            else if (tag.type == sl::kBufferTypeMotionVectors) type = XEFG_SWAPCHAIN_RES_MOTION_VECTOR;
            else if (tag.type == sl::kBufferTypeHUDLessColor) type = XEFG_SWAPCHAIN_RES_HUDLESS_COLOR;
            else if (tag.type == sl::kBufferTypeUIColorAndAlpha) type = XEFG_SWAPCHAIN_RES_UI;
            else continue;
            const auto* resource = tag.resource;
            if (!resource || !resource->native || resource->state == UINT_MAX) continue;
            const auto validity = xemfg_standalone::StreamlineValidity(uint32_t(tag.lifecycle), list != nullptr);
            if (validity == xemfg_standalone::Validity::eSkip) continue;
            auto* d3d = static_cast<ID3D12Resource*>(resource->native);
            const D3D12_RESOURCE_DESC desc = d3d->GetDesc();
            const auto area = xemfg_standalone::TagArea(tag.extent.left, tag.extent.top, tag.extent.width, tag.extent.height,
                                                        uint32_t(desc.Width), desc.Height);
            if (!Tag(*chain, list.Get(), id, type, d3d, area, resource->state, validity == xemfg_standalone::Validity::eOnlyNow)) continue;
            if (type == XEFG_SWAPCHAIN_RES_DEPTH) { record.depth = true; record.depthWidth = area.width; record.depthHeight = area.height; }
            else if (type == XEFG_SWAPCHAIN_RES_MOTION_VECTOR) { record.motion = true; record.motionWidth = area.width; record.motionHeight = area.height; }
            else if (type == XEFG_SWAPCHAIN_RES_HUDLESS_COLOR) record.hudless = true;
            else record.ui = true;
        }
        if (record.depth && record.motion) {
            if (gHost.viewport == UINT32_MAX) gHost.viewport = viewport;
            if (!gHost.streamlineMs || NowMs() - gHost.streamlineMs >= 1000) ++gInputFrames;
            gHost.streamlineMs = NowMs();
            LogSource(Source::eStreamline);
        }
    } catch (...) {}
}

void StreamlineConstants(const sl::Constants& constants, uint32_t frame, uint32_t viewport) noexcept
{
    if (!Wanted()) return;
    try {
        std::lock_guard lock(gHost.mutex);
        if (!ActiveLocked() || (gHost.viewport != UINT32_MAX && gHost.viewport != viewport)) return;
        if (frame > gHost.latestToken) gHost.latestToken = frame;
        const uint32_t id = gHost.ids.FromToken(frame);
        if (!id) { NoteStreamlineRefusedLocked("constants", frame); return; }
        auto& record = gHost.ring.At(id);
        if (!xemfg_standalone::Claim(record, Source::eStreamline)) return;
        const auto valid = [](float value) { return std::isfinite(value) && std::fabs(value) < 1e30f; };
        record.constants = true;
        record.jitterX = valid(constants.jitterOffset.x) ? constants.jitterOffset.x : 0.0f;
        record.jitterY = valid(constants.jitterOffset.y) ? constants.jitterOffset.y : 0.0f;
        record.motionScaleX = valid(constants.mvecScale.x) ? constants.mvecScale.x : 0.0f;
        record.motionScaleY = valid(constants.mvecScale.y) ? constants.mvecScale.y : 0.0f;
        record.motionNormalized = true;
        record.reset = constants.reset == sl::Boolean::eTrue;
        if (constants.depthInverted != sl::Boolean::eInvalid) {
            record.depthInvertedKnown = true;
            record.depthInverted = constants.depthInverted == sl::Boolean::eTrue;
            gObservedInverted = record.depthInverted;
        }
        record.cameraNear = valid(constants.cameraNear) ? constants.cameraNear : 0.0f;
        record.cameraFar = valid(constants.cameraFar) ? constants.cameraFar : 0.0f;
        record.cameraVerticalFov = valid(constants.cameraFOV) ? constants.cameraFOV : 0.0f;
    } catch (...) {}
}

void LatencyMarker(uint32_t marker, uint32_t frame) noexcept
{
    if (marker > XELL_PRESENT_END || !gEnabled.load()) return;
    try {
        std::lock_guard lock(gHost.mutex);
        if (!gHost.xell || !ActiveLocked()) return;
        if (frame > gHost.latestToken) gHost.latestToken = frame;
        const uint32_t id = gHost.ids.FromToken(frame);
        if (!id) return;
        gHost.markersMs = NowMs();
        ++gHost.markers;
        if (marker == XELL_PRESENT_START) { gHost.markerPresentId = id; gHost.presentToken = frame; }
        if (marker == XELL_RENDERSUBMIT_START) {
            gHost.markerBuildId = std::max(gHost.markerBuildId, id);
            gHost.renderMarkersMs = gHost.markersMs;
            gHost.renderToken = frame;
        }
        Marker(id, static_cast<xell_latency_marker_type_t>(marker));
    } catch (...) {}
}

bool ReplacesReflexSleep() noexcept
{
    if (!gEnabled.load()) return false;
    try {
        std::lock_guard lock(gHost.mutex);
        const auto chain = ActiveLocked();
        // xemfgsa.6: also at Dynamic MFG's 1X, which XeLL still paces (its FPS limit, the waits Dynamic MFG reads).
        return gHost.xell && chain && chain->fg && (chain->generating || chain->dynamicOff);
    } catch (...) {
        return false;
    }
}

void LatencySleep(uint32_t frame, bool slept) noexcept
{
    if (!gEnabled.load()) return;
    try {
        xell_context_handle_t xell = nullptr;
        uint32_t id = 0;
        {
            std::lock_guard lock(gHost.mutex);
            if (!gHost.xell || !ActiveLocked()) return;
            if (slept) { gHost.sleepMs = NowMs(); return; }
            id = gHost.ids.FromToken(frame);
            xell = gHost.xell;
            gHost.sleepMs = NowMs();   // the game's frames sleep here: the host adds no sleep of its own
            ApplyFpsLimitLocked();
        }
        // The game's Reflex sleep is XeLL's while XeMFG generates (ReplacesReflexSleep), or did not run (no Reflex on
        // this GPU): the frame sleeps in XeLL.
        if (xell && id) {
            const int64_t start = Now();
            {
                xess_mod::VulkanHostCallScope scope;
                gRuntime.xellSleep(xell, id);
            }
            std::lock_guard lock(gHost.mutex);
            gHost.sleepTicks += Now() - start;
            gHost.frameSleepTicks += Now() - start;   // xemfgsa.6
            ++gHost.reflexSleeps;
        }
    } catch (...) {}
}

// ---------------------------------------------------------------- DLSS Super Resolution (NGX D3D12 evaluate)
void CaptureDlssEvaluate(ID3D12GraphicsCommandList* list, const NVSDK_NGX_Parameter* parameters) noexcept
{
    if (!list || !parameters || !Wanted()) return;
    try {
        ID3D12Resource *depth = nullptr, *motion = nullptr, *color = nullptr, *output = nullptr;
        if (NVSDK_NGX_FAILED(parameters->Get(NVSDK_NGX_Parameter_Depth, &depth)) || !depth ||
            NVSDK_NGX_FAILED(parameters->Get(NVSDK_NGX_Parameter_MotionVectors, &motion)) || !motion ||
            NVSDK_NGX_FAILED(parameters->Get(NVSDK_NGX_Parameter_Color, &color)) || !color ||
            NVSDK_NGX_FAILED(parameters->Get(NVSDK_NGX_Parameter_Output, &output)) || !output)
            return;   // not an upscaling evaluation (frame generation, DLSS 5 and others carry other names)
        UpscalerFrame in;
        in.source = Source::eDlss;
        in.list = list;
        in.depth = depth;
        in.motion = motion;
        int flags = 0;
        bool lowRes = true;
        if (NVSDK_NGX_SUCCEED(parameters->Get(NVSDK_NGX_Parameter_DLSS_Feature_Create_Flags, &flags))) {
            in.invertedKnown = true;
            in.inverted = (flags & NVSDK_NGX_DLSS_Feature_Flags_DepthInverted) != 0;
            lowRes = (flags & NVSDK_NGX_DLSS_Feature_Flags_MVLowRes) != 0;
        }
        const D3D12_RESOURCE_DESC depthDesc = depth->GetDesc(), motionDesc = motion->GetDesc();
        unsigned int value = 0;
        uint32_t width = 0, height = 0;
        if (NVSDK_NGX_SUCCEED(parameters->Get(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Width, &value))) width = value;
        if (NVSDK_NGX_SUCCEED(parameters->Get(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Height, &value))) height = value;
        if (!width || !height) {
            if (NVSDK_NGX_SUCCEED(parameters->Get(NVSDK_NGX_Parameter_Width, &value))) width = value;
            if (NVSDK_NGX_SUCCEED(parameters->Get(NVSDK_NGX_Parameter_Height, &value))) height = value;
        }
        if (!width || !height) { width = uint32_t(depthDesc.Width); height = depthDesc.Height; }
        in.depthArea.width = std::min<uint32_t>(width, uint32_t(depthDesc.Width));
        in.depthArea.height = std::min<uint32_t>(height, depthDesc.Height);
        uint32_t outWidth = 0, outHeight = 0;
        if (NVSDK_NGX_SUCCEED(parameters->Get(NVSDK_NGX_Parameter_OutWidth, &value))) outWidth = value;
        if (NVSDK_NGX_SUCCEED(parameters->Get(NVSDK_NGX_Parameter_OutHeight, &value))) outHeight = value;
        if (lowRes || !outWidth || !outHeight) {
            in.motionArea.width = std::min<uint32_t>(in.depthArea.width, uint32_t(motionDesc.Width));
            in.motionArea.height = std::min<uint32_t>(in.depthArea.height, motionDesc.Height);
        } else {
            in.motionArea.width = std::min<uint32_t>(outWidth, uint32_t(motionDesc.Width));
            in.motionArea.height = std::min<uint32_t>(outHeight, motionDesc.Height);
        }
        float number = 0;
        if (NVSDK_NGX_SUCCEED(parameters->Get(NVSDK_NGX_Parameter_Jitter_Offset_X, &number))) in.jitterX = number;
        if (NVSDK_NGX_SUCCEED(parameters->Get(NVSDK_NGX_Parameter_Jitter_Offset_Y, &number))) in.jitterY = number;
        if (NVSDK_NGX_SUCCEED(parameters->Get(NVSDK_NGX_Parameter_MV_Scale_X, &number)) && number != 0) in.motionScaleX = number;
        if (NVSDK_NGX_SUCCEED(parameters->Get(NVSDK_NGX_Parameter_MV_Scale_Y, &number)) && number != 0) in.motionScaleY = number;
        int reset = 0;
        if (NVSDK_NGX_SUCCEED(parameters->Get(NVSDK_NGX_Parameter_Reset, &reset))) in.reset = reset != 0;
        TagUpscalerFrame(in);
    } catch (...) {}
}

// ---------------------------------------------------------------- XeSS Super Resolution (libxess, D3D12)
namespace
{
// libxess's public D3D12 structures (xess_d3d12.h, XeSS 1.x/2.x, 8-byte packing = natural alignment here).
struct XessCoord { uint32_t x, y; };
struct XessExecuteParams
{
    ID3D12Resource* color;
    ID3D12Resource* velocity;
    ID3D12Resource* depth;
    ID3D12Resource* exposureScale;
    ID3D12Resource* responsiveMask;
    ID3D12Resource* output;
    float jitterOffsetX, jitterOffsetY;
    float exposureScaleValue;
    uint32_t resetHistory;
    uint32_t inputWidth, inputHeight;
    XessCoord inputColorBase, inputMotionVectorBase, inputDepthBase, inputResponsiveMaskBase, reserved0, outputColorBase;
    ID3D12DescriptorHeap* descriptorHeap;
    uint32_t descriptorHeapOffset;
};
static_assert(offsetof(XessExecuteParams, inputColorBase) == 72 && sizeof(XessExecuteParams) == 136);
struct XessInitParams
{
    XessCoord outputResolution;
    int32_t qualitySetting;
    uint32_t initFlags;
};
constexpr uint32_t kXessHighResMv = 1u << 0, kXessInvertedDepth = 1u << 1, kXessNdcVelocity = 1u << 4;
struct XessVersion { uint16_t major, minor, patch, reserved; };
using XessExecuteFn = int32_t(*)(void* context, ID3D12GraphicsCommandList* list, const XessExecuteParams* params);
using XessGetInitParamsFn = int32_t(*)(void* context, void* params);
using XessGetScaleFn = int32_t(*)(void* context, float* x, float* y);
using XessGetVersionFn = int32_t(*)(XessVersion* version);

struct XessModule { HMODULE module; XessExecuteFn execute; XessGetInitParamsFn initParams; XessGetScaleFn velocityScale; };
std::mutex gHookMutex;
std::vector<XessModule> gXess;
std::unordered_set<void*> gHooked;

template<size_t Slot> int32_t XessExecuteHook(void* context, ID3D12GraphicsCommandList* list, const XessExecuteParams* params)
{
    XessModule module{};
    {
        std::lock_guard lock(gHookMutex);
        if (Slot < gXess.size()) module = gXess[Slot];
    }
    if (!module.execute) return -1;
    if (params && list && params->depth && params->velocity && Wanted()) {
        try {
            alignas(16) uint8_t initBuffer[256]{};   // a later libxess may write a longer structure
            uint32_t flags = 0;
            bool flagsKnown = false;
            if (module.initParams && module.initParams(context, initBuffer) == 0) {
                flags = reinterpret_cast<const XessInitParams*>(initBuffer)->initFlags;
                flagsKnown = true;
            }
            float scaleX = 1, scaleY = 1;
            if (module.velocityScale) module.velocityScale(context, &scaleX, &scaleY);
            UpscalerFrame in;
            in.source = Source::eXess;
            in.list = list;
            in.depth = params->depth;
            in.motion = params->velocity;
            in.depthArea = {params->inputDepthBase.x, params->inputDepthBase.y, params->inputWidth, params->inputHeight};
            in.motionArea = {params->inputMotionVectorBase.x, params->inputMotionVectorBase.y, params->inputWidth, params->inputHeight};
            if (flags & kXessHighResMv) {
                const auto* init = reinterpret_cast<const XessInitParams*>(initBuffer);
                in.motionArea.width = init->outputResolution.x;
                in.motionArea.height = init->outputResolution.y;
            }
            const auto scale = xemfg_standalone::XessMotionScale(scaleX, scaleY, (flags & kXessNdcVelocity) != 0,
                                                                 in.motionArea.width, in.motionArea.height);
            in.motionScaleX = scale.first;
            in.motionScaleY = scale.second;
            in.jitterX = params->jitterOffsetX;
            in.jitterY = params->jitterOffsetY;
            in.reset = params->resetHistory != 0;
            in.invertedKnown = flagsKnown;
            in.inverted = (flags & kXessInvertedDepth) != 0;
            TagUpscalerFrame(in);
        } catch (...) {}
    }
    return module.execute(context, list, params);
}
constexpr XessExecuteFn kXessHooks[] = {&XessExecuteHook<0>, &XessExecuteHook<1>};

// ---------------------------------------------------------------- FSR 3.1+ (FidelityFX API, D3D12 modules)
// The FidelityFX API's public structures (ffx_api.h, ffx_upscale.h, ffx_framegeneration.h; MIT, AMD).
struct FfxHeader { uint64_t type; FfxHeader* next; };
struct FfxResourceDescription { uint32_t type, format, width, height, depth, mipCount, flags, usage; };
struct FfxResource { void* resource; FfxResourceDescription description; uint32_t state; };
struct FfxFloat2 { float x, y; };
struct FfxDimensions { uint32_t width, height; };
struct FfxDispatchUpscale
{
    FfxHeader header;
    void* commandList;
    FfxResource color, depth, motionVectors, exposure, reactive, transparencyAndComposition, output;
    FfxFloat2 jitterOffset, motionVectorScale;
    FfxDimensions renderSize, upscaleSize;
    bool enableSharpening;
    float sharpness, frameTimeDelta, preExposure;
    bool reset;
    float cameraNear, cameraFar, cameraFovAngleVertical, viewSpaceToMetersFactor;
    uint32_t flags;
};
struct FfxCreateUpscale { FfxHeader header; uint32_t flags; FfxDimensions maxRenderSize, maxUpscaleSize; void* message; };
struct FfxConfigureFrameGeneration
{
    FfxHeader header;
    void* swapChain;
    void* presentCallback;
    void* presentCallbackUserContext;
    void* frameGenerationCallback;
    void* frameGenerationCallbackUserContext;
    bool frameGenerationEnabled;
};
constexpr uint64_t kFfxCreateUpscale = 0x00010000u, kFfxDispatchUpscale = 0x00010001u, kFfxConfigureFrameGeneration = 0x00020002u;
constexpr uint32_t kFfxDisplayResolutionMv = 1u << 1, kFfxDepthInverted = 1u << 3, kFfxDepthInfinite = 1u << 4;
using FfxCreateFn = uint32_t(*)(void** context, FfxHeader* desc, const void* callbacks);
using FfxDestroyFn = uint32_t(*)(void** context, const void* callbacks);
using FfxConfigureFn = uint32_t(*)(void** context, const FfxHeader* desc);
using FfxDispatchFn = uint32_t(*)(void** context, const FfxHeader* desc);

struct FfxModule { HMODULE module; FfxCreateFn create; FfxDestroyFn destroy; FfxConfigureFn configure; FfxDispatchFn dispatch; };
std::vector<FfxModule> gFfx;
std::unordered_map<void*, uint32_t> gFfxFlags;   // upscale context -> its create flags

// The chain entry of `type`, bounded and fault-safe (no C++ objects in this frame).
const FfxHeader* FindFfx(const FfxHeader* desc, uint64_t type) noexcept
{
    __try {
        for (int i = 0; desc && i < 8; ++i, desc = desc->next)
            if (desc->type == type) return desc;
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    return nullptr;
}

template<size_t Slot> FfxModule FfxAt()
{
    std::lock_guard lock(gHookMutex);
    return Slot < gFfx.size() ? gFfx[Slot] : FfxModule{};
}

template<size_t Slot> uint32_t FfxCreateHook(void** context, FfxHeader* desc, const void* callbacks)
{
    const auto module = FfxAt<Slot>();
    if (!module.create) return 1;
    const uint32_t result = module.create(context, desc, callbacks);
    if (result == 0 && context && *context) {
        if (const auto* upscale = reinterpret_cast<const FfxCreateUpscale*>(FindFfx(desc, kFfxCreateUpscale))) {
            std::lock_guard lock(gHookMutex);
            gFfxFlags[*context] = upscale->flags;
        }
    }
    return result;
}

template<size_t Slot> uint32_t FfxDestroyHook(void** context, const void* callbacks)
{
    const auto module = FfxAt<Slot>();
    if (!module.destroy) return 1;
    if (context && *context) { std::lock_guard lock(gHookMutex); gFfxFlags.erase(*context); }
    return module.destroy(context, callbacks);
}

template<size_t Slot> uint32_t FfxConfigureHook(void** context, const FfxHeader* desc)
{
    const auto module = FfxAt<Slot>();
    if (!module.configure) return 1;
    if (const auto* fg = reinterpret_cast<const FfxConfigureFrameGeneration*>(FindFfx(desc, kFfxConfigureFrameGeneration))) {
        const bool on = fg->frameGenerationEnabled;
        if (gFsrFrameGeneration.exchange(on) != on)
            Log("d3d12_game_fsr_frame_generation", std::string("\"on\":") + (on ? "true" : "false"));
    }
    return module.configure(context, desc);
}

template<size_t Slot> uint32_t FfxDispatchHook(void** context, const FfxHeader* desc)
{
    const auto module = FfxAt<Slot>();
    if (!module.dispatch) return 1;
    const auto* upscale = Wanted() ? reinterpret_cast<const FfxDispatchUpscale*>(FindFfx(desc, kFfxDispatchUpscale)) : nullptr;
    if (upscale && upscale->commandList && upscale->depth.resource && upscale->motionVectors.resource) {
        try {
            uint32_t flags = 0;
            bool flagsKnown = false;
            {
                std::lock_guard lock(gHookMutex);
                if (context) if (auto found = gFfxFlags.find(*context); found != gFfxFlags.end()) { flags = found->second; flagsKnown = true; }
            }
            UpscalerFrame in;
            in.source = Source::eFsr;
            in.list = static_cast<ID3D12GraphicsCommandList*>(upscale->commandList);
            in.depth = static_cast<ID3D12Resource*>(upscale->depth.resource);
            in.motion = static_cast<ID3D12Resource*>(upscale->motionVectors.resource);
            bool known = xemfg_standalone::FfxStateToD3d12(upscale->depth.state, in.depthState) &&
                         xemfg_standalone::FfxStateToD3d12(upscale->motionVectors.state, in.motionState);
            in.depthArea.width = upscale->renderSize.width;
            in.depthArea.height = upscale->renderSize.height;
            const D3D12_RESOURCE_DESC motionDesc = in.motion->GetDesc();
            // Display-resolution vectors: the create flag, or (context created before our hook) a motion-vector
            // resource wider than the render size and as wide as the output.
            const bool displayMv = flagsKnown ? (flags & kFfxDisplayResolutionMv) != 0
                : upscale->upscaleSize.width && uint32_t(motionDesc.Width) >= upscale->upscaleSize.width &&
                  upscale->upscaleSize.width > upscale->renderSize.width;
            in.motionArea = displayMv ? xemfg_standalone::Area{0, 0, upscale->upscaleSize.width, upscale->upscaleSize.height}
                                      : in.depthArea;
            in.jitterX = upscale->jitterOffset.x;
            in.jitterY = upscale->jitterOffset.y;
            in.motionScaleX = upscale->motionVectorScale.x;
            in.motionScaleY = upscale->motionVectorScale.y;
            in.reset = upscale->reset;
            in.invertedKnown = flagsKnown;
            in.inverted = (flags & kFfxDepthInverted) != 0;
            in.cameraNear = upscale->cameraNear;
            in.cameraFar = flagsKnown && (flags & kFfxDepthInfinite) ? INFINITY : upscale->cameraFar;
            in.cameraVerticalFov = upscale->cameraFovAngleVertical;
            in.frameTimeMs = upscale->frameTimeDelta;
            if (known) TagUpscalerFrame(in);
        } catch (...) {}
    }
    return module.dispatch(context, desc);
}

constexpr FfxCreateFn kFfxCreateHooks[] = {&FfxCreateHook<0>, &FfxCreateHook<1>, &FfxCreateHook<2>, &FfxCreateHook<3>};
constexpr FfxDestroyFn kFfxDestroyHooks[] = {&FfxDestroyHook<0>, &FfxDestroyHook<1>, &FfxDestroyHook<2>, &FfxDestroyHook<3>};
constexpr FfxConfigureFn kFfxConfigureHooks[] = {&FfxConfigureHook<0>, &FfxConfigureHook<1>, &FfxConfigureHook<2>, &FfxConfigureHook<3>};
constexpr FfxDispatchFn kFfxDispatchHooks[] = {&FfxDispatchHook<0>, &FfxDispatchHook<1>, &FfxDispatchHook<2>, &FfxDispatchHook<3>};

bool HookExport(HMODULE module, const char* name, void* detour, void** original)
{
    void* target = reinterpret_cast<void*>(GetProcAddress(module, name));
    HMODULE owner = nullptr;
    if (!target || gHooked.contains(target) ||
        !GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            static_cast<LPCWSTR>(target), &owner) || owner != module)
        return false;
    if (MH_CreateHook(target, detour, original) != MH_OK) return false;
    if (MH_EnableHook(target) != MH_OK) { MH_RemoveHook(target); return false; }
    gHooked.insert(target);
    return true;
}

std::wstring BaseName(HMODULE module)
{
    wchar_t name[MAX_PATH]{};
    return GetModuleBaseNameW(GetCurrentProcess(), module, name, MAX_PATH) ? std::wstring(name) : std::wstring();
}

void HookXess(HMODULE module)
{
    auto version = reinterpret_cast<XessGetVersionFn>(GetProcAddress(module, "xessGetVersion"));
    XessVersion v{};
    if (!version || version(&v) != 0 || v.major < 1 || v.major > 2) {
        Log("d3d12_upscaler_refused", "\"upscaler\":\"xess\",\"version\":\"" + std::to_string(v.major) + "." + std::to_string(v.minor) + "\"");
        return;
    }
    const size_t slot = gXess.size();
    if (slot >= std::size(kXessHooks)) return;
    XessModule entry{module};
    entry.initParams = reinterpret_cast<XessGetInitParamsFn>(GetProcAddress(module, "xessD3D12GetInitParams"));
    entry.velocityScale = reinterpret_cast<XessGetScaleFn>(GetProcAddress(module, "xessGetVelocityScale"));
    void* original = nullptr;
    gXess.push_back(entry);
    if (!HookExport(module, "xessD3D12Execute", reinterpret_cast<void*>(kXessHooks[slot]), &original)) { gXess.pop_back(); return; }
    gXess[slot].execute = reinterpret_cast<XessExecuteFn>(original);
    Log("d3d12_upscaler_hooked", "\"upscaler\":\"xess\",\"module\":\"" + Escape(Narrow(BaseName(module))) + "\",\"version\":\"" +
        std::to_string(v.major) + "." + std::to_string(v.minor) + "." + std::to_string(v.patch) + "\"");
}

void HookFfx(HMODULE module)
{
    const size_t slot = gFfx.size();
    if (slot >= std::size(kFfxDispatchHooks)) return;
    gFfx.push_back(FfxModule{module});
    void *create = nullptr, *destroy = nullptr, *configure = nullptr, *dispatch = nullptr;
    // Creation and configuration first: the dispatch reads their records.
    const bool hooked = HookExport(module, "ffxCreateContext", reinterpret_cast<void*>(kFfxCreateHooks[slot]), &create) &&
                        HookExport(module, "ffxDestroyContext", reinterpret_cast<void*>(kFfxDestroyHooks[slot]), &destroy) &&
                        HookExport(module, "ffxConfigure", reinterpret_cast<void*>(kFfxConfigureHooks[slot]), &configure);
    gFfx[slot].create = reinterpret_cast<FfxCreateFn>(create);
    gFfx[slot].destroy = reinterpret_cast<FfxDestroyFn>(destroy);
    gFfx[slot].configure = reinterpret_cast<FfxConfigureFn>(configure);
    if (hooked && HookExport(module, "ffxDispatch", reinterpret_cast<void*>(kFfxDispatchHooks[slot]), &dispatch))
        gFfx[slot].dispatch = reinterpret_cast<FfxDispatchFn>(dispatch);
    // A partial set keeps its trampolines (they forward); its slot stays taken.
    Log("d3d12_upscaler_hooked", "\"upscaler\":\"fsr\",\"module\":\"" + Escape(Narrow(BaseName(module))) + "\",\"complete\":" +
        (gFfx[slot].dispatch ? "true" : "false"));
}
} // namespace

void DiscoverUpscalers() noexcept
{
    if (!gEnabled.load() || optiscaler_host::Detected()) return;
    try {
        std::array<HMODULE, 2048> modules{};
        DWORD bytes = 0;
        if (!EnumProcessModulesEx(GetCurrentProcess(), modules.data(), sizeof(modules), &bytes, LIST_MODULES_64BIT) ||
            bytes > sizeof(modules))
            return;
        static std::unordered_set<HMODULE> seen;
        std::lock_guard lock(gHookMutex);
        const auto init = MH_Initialize();
        if (init != MH_OK && init != MH_ERROR_ALREADY_INITIALIZED) return;
        for (size_t i = 0; i < bytes / sizeof(HMODULE); ++i) {
            const HMODULE module = modules[i];
            if (!module || module == single_module::Self() || seen.contains(module)) continue;
            const bool xess = GetProcAddress(module, "xessD3D12Execute") && GetProcAddress(module, "xessGetVersion");
            std::wstring name;
            bool ffx = false;
            if (!xess && GetProcAddress(module, "ffxDispatch") && GetProcAddress(module, "ffxCreateContext")) {
                name = BaseName(module);
                for (auto& c : name) c = wchar_t(towlower(c));
                ffx = name.rfind(L"amd_fidelityfx", 0) == 0 && name.find(L"dx12") != std::wstring::npos;
            }
            if (!xess && !ffx) continue;
            // Hooked modules stay loaded for the process (our trampolines point into them).
            HMODULE pinned = nullptr;
            if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                                    reinterpret_cast<LPCWSTR>(module), &pinned) || pinned != module)
                continue;
            seen.insert(module);
            if (xess) HookXess(module);
            else HookFfx(module);
        }
    } catch (...) {}
}

// ---------------------------------------------------------------- status
Snapshot ReadSnapshot() noexcept
{
    Snapshot snapshot;
    try {
        snapshot.enabled = gEnabled.load();
        snapshot.gameFrameGeneration = gGameFrameGeneration.load();
        snapshot.dlssgSeen = gDlssgSeen.load();
        snapshot.fsrFrameGeneration = gFsrFrameGeneration.load();
        snapshot.basePresents = gBasePresents.load();
        snapshot.inputFrames = gInputFrames.load();
        if (gRuntimeTried.load()) snapshot.runtime = Narrow(gRuntime.folder);
        // alpha.15 (B4-12): where the pair belongs and the near-miss name, for the menu's note (UTF-8).
        if (gRuntimeTried.load() && gRuntimeFailed.load()) {
            snapshot.runtimeSearch = xess_runtime_search::Utf8(gRuntime.search.recommended.wstring());
            snapshot.runtimeGame = xess_runtime_search::Utf8(gRuntime.search.gameFolder.wstring());
            snapshot.runtimeMisnamed = xess_runtime_search::Utf8(gRuntime.search.misnamed);
        }
        std::lock_guard lock(gHost.mutex);
        snapshot.source = uint32_t(gHost.lastSource);
        snapshot.reason = gHost.reason;
        if (auto chain = ActiveLocked()) {
            snapshot.active = true;
            // xemfgsa.6: Dynamic MFG's 1X (generation off) is XeMFG at work, at one presented frame per game frame.
            snapshot.generating = chain->generating || chain->dynamicOff;
            snapshot.frames = chain->dynamicOff ? 1 : chain->framesApplied;
            snapshot.dynamic = chain->dynamicRan;
            snapshot.dynamicCap = chain->dynamicRan && chain->dynamic.CapActive();
            snapshot.dynamicTargetMilli = chain->dynamicRan ? uint32_t(chain->dynamic.TargetFps() * 1000.0 + 0.5) : 0;
        }
        // xemfgsa.5
        snapshot.baseFpsMilli = gHost.fps.baseMilli;
        snapshot.outputFpsMilli = gHost.fps.outputMilli;
        snapshot.fpsAgeMs = gHost.fps.sampleMs ? NowMs() - gHost.fps.sampleMs : UINT64_MAX;
        snapshot.fpsLimit = gFpsLimit.load();
        snapshot.limitIntervalUs = gHost.limitIntervalUs;
        snapshot.restartNeeded = snapshot.enabled && !snapshot.active && gNativeBeforeOn.load();
        snapshot.state = !snapshot.enabled ? "off"
            : optiscaler_host::Detected() ? "optiscaler"
            : snapshot.active ? (snapshot.generating ? "active" : "waiting")
            : gRuntimeFailed.load() ? "unavailable"
            : snapshot.restartNeeded ? "restart"
            : "waiting";
        if (gRuntimeFailed.load() && snapshot.reason.empty()) snapshot.reason = gRuntime.reason;
    } catch (...) {}
    return snapshot;
}
}
