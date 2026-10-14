#include "present_capture.h"
#include "xess_mod.h"
#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace xess_capture
{
namespace
{
using Microsoft::WRL::ComPtr;
using sdk_pacing::OutputKind;
// About a second of countdown after the hotkey (time to start a camera pan), then kBatches captured
// (~1.5 s at ~25 fps base), written. A beep marks the capture start and the finished write.
constexpr uint32_t kBatches = 36, kCountdownBatches = 30, kSlots = 240;
constexpr uint32_t kMotionFrames = 3, kDepthFrames = 1, kHudlessFrames = 1, kMaxConstants = 128;
constexpr UINT kCropWidth = 768, kCropHeight = 432, kCornerWidth = 384, kCornerHeight = 216;
enum State : int { eIdle = 0, eRequested = 1, ePreparing = 2, eCountdown = 3, eCapturing = 4, eWriting = 5 };

// beta.8: one pass with the startup input fixes (xess_mod.h); the model (ported MFG or 2x) is chosen
// at startup. The motion files hold what XeFG received; hudless0.bin is the HUD-less input XeFG received
// (by default our converted texture, in the back buffer's format).
constexpr const char* kPass = "capture";

std::atomic<int> gState{eIdle};
std::atomic<bool> gEnabled{};
std::once_flag gStartOnce;
std::wstring gRoot; // output root (default %TEMP%)
HANDLE gWake{};
std::mutex gTargetMutex;
std::wstring gSession; // writer thread

struct Target
{
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12CommandQueue> queue;
    D3D12_COMMAND_LIST_TYPE type{};
    DXGI_FORMAT format{};
    UINT width{}, height{};
    D3D12_BOX box{}, cornerBox{};
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{}, cornerFootprint{};
    UINT64 bytes{}, cornerBytes{};
};
Target gTarget; // set under gTargetMutex while requested; read-only from ePreparing until idle

struct Slot
{
    ComPtr<ID3D12Resource> buffer, corner;
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> list;
};
std::vector<Slot> gSlots; // built by the writer before the countdown, released after the write
struct FrameMeta { bool valid{}; int kind{}; uint32_t presentId{}, index{}, batch{}; UINT backBuffer{}; int64_t ns{}; };
std::array<FrameMeta, kSlots> gMeta;
std::atomic<uint32_t> gUsed{}, gCountdown{}, gBatch{}, gIndexInBatch{};
ComPtr<ID3D12Fence> gFence;
std::atomic<uint64_t> gFenceBase{}; // fence value before the capture's first copy
std::atomic<uint64_t> gSignaled{};  // highest fence value queued (a slot may fail before its Signal)
HANDLE gFenceEvent{};

// The game's own inputs, copied in its command list at its XeFG resource tag.
struct InputCopy
{
    ComPtr<ID3D12Resource> buffer;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    UINT64 bytes{};
    uint32_t presentId{};
    bool recorded{};
};
struct InputKind
{
    xefg_swapchain_resource_type_t type;
    const wchar_t* prefix;
    std::vector<InputCopy> copies;
    std::atomic<uint32_t> used{};
    D3D12_RESOURCE_DESC desc{};
    D3D12_RESOURCE_STATES state{};
    bool known{};
};
std::mutex gInputMutex;
InputKind gMotion{XEFG_SWAPCHAIN_RES_MOTION_VECTOR, L"mv"};
InputKind gDepth{XEFG_SWAPCHAIN_RES_DEPTH, L"depth"};
InputKind gHudless{XEFG_SWAPCHAIN_RES_HUDLESS_COLOR, L"hudless"}; // dev.46

std::mutex gConstantsMutex;
std::vector<std::pair<uint32_t, xefg_swapchain_frame_constant_data_t>> gConstants;
std::mutex gFolderMutex;
std::wstring gLastFolder;

struct Current { bool valid{}; OutputKind kind{OutputKind::eOther}; uint32_t presentId{}; ID3D12CommandQueue* queue{}; };
thread_local Current tCurrent;

void Log(const char* stage, const std::string& fields) noexcept
{
    try { xess_mod::WriteLog(stage, fields); } catch (...) {}
}

int64_t NowNs() noexcept
{
    LARGE_INTEGER counter{}, frequency{};
    if (!QueryPerformanceCounter(&counter) || !QueryPerformanceFrequency(&frequency) || frequency.QuadPart <= 0) return 0;
    return int64_t(double(counter.QuadPart)*1e9/double(frequency.QuadPart));
}

std::string Narrow(const std::wstring& text)
{
    std::string result;
    for (wchar_t c : text) result.push_back(c == L'\\' ? '/' : c < 0x80 && c != L'"' ? char(c) : '?');
    return result;
}

std::string Number(double value)
{
    if (!std::isfinite(value)) return "null";
    char text[32]{};
    sprintf_s(text, "%.9g", value);
    return text;
}

std::string Matrix(const float (&m)[16])
{
    std::string text = "[";
    for (int i = 0; i < 16; ++i) text += (i ? "," : "")+Number(m[i]);
    return text+"]";
}

// The input fixes in effect (read once at startup; xess_mod.h).
std::string InputFixes()
{
    static const char* const kMotion[] = {"game", "fill", "zero"};
    const uint32_t motion = xess_mod::MotionFillDefault();
    return std::string("{\"matrix_fix\":")+(xess_mod::MatrixFixDefault() ? "true" : "false")+
        ",\"motion_vectors\":\""+(motion <= 2 ? kMotion[motion] : "unknown")+
        "\",\"ui_composition\":\""+(xess_mod::UiCompositionOffDefault() ? "off" : "game")+"\",\"hudless\":\""+
        (xess_mod::HudlessMatchDefault() == 1 ? "match" : "game")+"\"}";
}

const char* KindName(int kind) noexcept
{
    switch (OutputKind(kind))
    {
    case OutputKind::eFirst: return "first";
    case OutputKind::eExtra: return "extra";
    case OutputKind::eReal: return "real";
    default: return "other";
    }
}

std::wstring ModuleDirectoryFile(const wchar_t* name)
{
    wchar_t path[MAX_PATH]{};
    const DWORD length = GetModuleFileNameW(xess_mod::Self(), path, MAX_PATH);
    if (!length || length >= MAX_PATH) return {};
    wchar_t* slash = wcsrchr(path, L'\\');
    if (!slash || size_t(slash+1-path)+wcslen(name) >= MAX_PATH) return {};
    wcscpy_s(slash+1, MAX_PATH-size_t(slash+1-path), name);
    return path;
}

void ResetCapture() noexcept
{
    gUsed = 0;
    for (auto& meta : gMeta) meta = {};
    for (auto* kind : {&gMotion, &gDepth, &gHudless})
    {
        kind->used = 0;
        for (auto& copy : kind->copies) { copy.recorded = false; copy.presentId = 0; }
    }
    { std::lock_guard lock(gConstantsMutex); gConstants.clear(); }
}

void ReleaseResources() noexcept
{
    gSlots.clear();
    gFence.Reset();
    {
        std::lock_guard lock(gInputMutex);
        for (auto* kind : {&gMotion, &gDepth, &gHudless}) { kind->copies.clear(); kind->known = false; kind->used = 0; }
    }
    { std::lock_guard lock(gTargetMutex); gTarget = {}; }
    ResetCapture();
}

D3D12_HEAP_PROPERTIES ReadbackHeap() noexcept
{
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_READBACK;
    return heap;
}

D3D12_RESOURCE_DESC BufferDesc(UINT64 bytes) noexcept
{
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = bytes; desc.Height = 1; desc.DepthOrArraySize = 1; desc.MipLevels = 1;
    desc.SampleDesc.Count = 1; desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    return desc;
}

bool CreateReadback(ID3D12Device* device, UINT64 bytes, ComPtr<ID3D12Resource>& buffer)
{
    const auto heap = ReadbackHeap();
    const auto desc = BufferDesc(bytes);
    return SUCCEEDED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
        D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&buffer)));
}

// The input's description comes from the game's next resource tag (up to 1 s).
void AllocateInput(ID3D12Device* device, InputKind& kind, uint32_t count)
{
    D3D12_RESOURCE_DESC desc{};
    bool known{};
    for (int i = 0; i < 100 && !known; ++i)
    {
        { std::lock_guard lock(gInputMutex); known = kind.known; desc = kind.desc; }
        if (!known) Sleep(10);
    }
    if (!known) return;
    std::vector<InputCopy> copies(count);
    for (auto& copy : copies)
    {
        device->GetCopyableFootprints(&desc, 0, 1, 0, &copy.footprint, nullptr, nullptr, &copy.bytes);
        if (!copy.bytes || copy.bytes > 256ull*1024*1024 || !CreateReadback(device, copy.bytes, copy.buffer)) copy.buffer.Reset();
    }
    std::lock_guard lock(gInputMutex);
    kind.copies = std::move(copies);
}

// Writer thread: allocation for a request, files and release after a capture.
bool Allocate() noexcept
{
    try
    {
        const Target& target = gTarget;
        gSlots.assign(kSlots, {});
        for (auto& slot : gSlots)
        {
            if (!CreateReadback(target.device.Get(), target.bytes, slot.buffer)
                || !CreateReadback(target.device.Get(), target.cornerBytes, slot.corner)
                || FAILED(target.device->CreateCommandAllocator(target.type, IID_PPV_ARGS(&slot.allocator)))
                || FAILED(target.device->CreateCommandList(0, target.type, slot.allocator.Get(), nullptr, IID_PPV_ARGS(&slot.list)))
                || FAILED(slot.list->Close())) return false;
        }
        if (FAILED(target.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&gFence)))) return false;
        gFenceBase = 0; gSignaled = 0;
        AllocateInput(target.device.Get(), gMotion, kMotionFrames);
        AllocateInput(target.device.Get(), gDepth, kDepthFrames);
        AllocateInput(target.device.Get(), gHudless, kHudlessFrames);
        Log("capture_ready", "\"slots\":"+std::to_string(kSlots)+",\"frame_bytes\":"+std::to_string(target.bytes)+
            ",\"motion\":"+(gMotion.copies.empty() ? "false" : "true")+",\"depth\":"+(gDepth.copies.empty() ? "false" : "true"));
        return true;
    }
    catch (...) { return false; }
}

std::wstring SessionFolder()
{
    if (!gSession.empty()) return gSession;
    std::wstring root = gRoot;
    if (root.empty())
    {
        wchar_t temp[MAX_PATH]{};
        const DWORD length = GetTempPathW(MAX_PATH, temp);
        root = length && length < MAX_PATH ? temp : L".\\";
    }
    SYSTEMTIME time{};
    GetLocalTime(&time);
    wchar_t name[64]{};
    swprintf_s(name, L"RTXMFG-capture-%04u%02u%02u-%02u%02u%02u", time.wYear, time.wMonth, time.wDay,
        time.wHour, time.wMinute, time.wSecond);
    gSession = (std::filesystem::path(root)/name).wstring();
    return gSession;
}

bool CopyRows(std::ofstream& file, ID3D12Resource* buffer, const D3D12_PLACED_SUBRESOURCE_FOOTPRINT& footprint, UINT64 bytes)
{
    void* data{};
    const D3D12_RANGE read{0, SIZE_T(bytes)};
    if (!buffer || FAILED(buffer->Map(0, &read, &data)) || !data) return false;
    const auto* base = static_cast<const char*>(data)+footprint.Offset;
    for (UINT y = 0; y < footprint.Footprint.Height; ++y)
        file.write(base+size_t(y)*footprint.Footprint.RowPitch, footprint.Footprint.RowPitch);
    const D3D12_RANGE written{0, 0};
    buffer->Unmap(0, &written);
    return bool(file);
}

std::string WriteInputs(const std::filesystem::path& folder, InputKind& kind)
{
    std::string list = "[";
    D3D12_RESOURCE_DESC desc{};
    D3D12_RESOURCE_STATES state{};
    { std::lock_guard lock(gInputMutex); desc = kind.desc; state = kind.state; }
    for (size_t i = 0; i < kind.copies.size(); ++i)
    {
        auto& copy = kind.copies[i];
        if (!copy.recorded || !copy.buffer) continue;
        const std::wstring name = std::wstring(kind.prefix)+std::to_wstring(i)+L".bin";
        std::ofstream file(folder/name, std::ios::binary);
        if (!CopyRows(file, copy.buffer.Get(), copy.footprint, copy.bytes)) continue;
        list += std::string(list.size() > 1 ? "," : "")+"{\"file\":\""+Narrow(name)+"\",\"present_id\":"+
            std::to_string(copy.presentId)+",\"format\":"+std::to_string(int(desc.Format))+",\"width\":"+
            std::to_string(copy.footprint.Footprint.Width)+",\"height\":"+std::to_string(copy.footprint.Footprint.Height)+
            ",\"row_pitch\":"+std::to_string(copy.footprint.Footprint.RowPitch)+",\"state\":"+std::to_string(int(state))+"}";
    }
    return list+"]";
}

void Write() noexcept
{
    try
    {
        const uint32_t used = (std::min)(gUsed.load(), kSlots);
        const uint64_t last = gSignaled.load();
        if (gFence && gFence->GetCompletedValue() < last)
        {
            if (!gFenceEvent) gFenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
            if (gFenceEvent && SUCCEEDED(gFence->SetEventOnCompletion(last, gFenceEvent)))
                WaitForSingleObject(gFenceEvent, 5000);
        }
        Sleep(500); // the input copies run in the game's own command lists
        const bool complete = !gFence || gFence->GetCompletedValue() >= last;
        const std::filesystem::path folder = std::filesystem::path(SessionFolder())/kPass;
        std::filesystem::create_directories(folder);
        const Target& target = gTarget;
        uint32_t written{};
        {
            std::ofstream frames(folder/L"frames.bin", std::ios::binary), corners(folder/L"corners.bin", std::ios::binary);
            for (uint32_t i = 0; i < used; ++i)
            {
                if (!gMeta[i].valid) continue;
                if (!CopyRows(frames, gSlots[i].buffer.Get(), target.footprint, target.bytes)
                    || !CopyRows(corners, gSlots[i].corner.Get(), target.cornerFootprint, target.cornerBytes))
                { gMeta[i].valid = false; continue; }
                ++written;
            }
        }
        const std::string motion = WriteInputs(folder, gMotion), depth = WriteInputs(folder, gDepth),
            hudless = WriteInputs(folder, gHudless);
        std::string frames = "[";
        uint32_t listed{};
        for (uint32_t i = 0; i < used; ++i)
        {
            const auto& meta = gMeta[i];
            if (!meta.valid) continue;
            frames += std::string(listed++ ? "," : "")+"{\"present_id\":"+std::to_string(meta.presentId)+",\"kind\":\""+
                KindName(meta.kind)+"\",\"index\":"+std::to_string(meta.index)+",\"batch\":"+std::to_string(meta.batch)+
                ",\"back_buffer\":"+std::to_string(meta.backBuffer)+",\"t_ns\":"+std::to_string(meta.ns)+"}";
        }
        frames += "]";
        std::string constants = "[";
        {
            std::lock_guard lock(gConstantsMutex);
            for (size_t i = 0; i < gConstants.size(); ++i)
            {
                const auto& [id, c] = gConstants[i];
                constants += std::string(i ? "," : "")+"{\"present_id\":"+std::to_string(id)+",\"view\":"+Matrix(c.viewMatrix)+
                    ",\"projection\":"+Matrix(c.projectionMatrix)+",\"jitter\":["+Number(c.jitterOffsetX)+","+Number(c.jitterOffsetY)+
                    "],\"mv_scale\":["+Number(c.motionVectorScaleX)+","+Number(c.motionVectorScaleY)+"],\"reset\":"+
                    std::to_string(c.resetHistory)+",\"frame_time\":"+Number(c.frameRenderTime)+"}";
            }
        }
        constants += "]";
        {
            std::ofstream index(folder/L"index.json", std::ios::binary);
            index << "{\"version\":3,\"build\":\"1.5.0\",\"variant\":{\"name\":\"" << kPass << "\",\"settings\":"
                << InputFixes() << "},\"format\":" << int(target.format) << ",\"width\":"
                << target.width << ",\"height\":" << target.height << ",\"crop\":{\"x\":" << target.box.left << ",\"y\":"
                << target.box.top << ",\"w\":" << target.footprint.Footprint.Width << ",\"h\":" << target.footprint.Footprint.Height
                << "},\"row_pitch\":" << target.footprint.Footprint.RowPitch << ",\"frame_bytes\":"
                << UINT64(target.footprint.Footprint.RowPitch)*target.footprint.Footprint.Height
                << ",\"corner\":{\"x\":" << target.cornerBox.left << ",\"y\":" << target.cornerBox.top << ",\"w\":"
                << target.cornerFootprint.Footprint.Width << ",\"h\":" << target.cornerFootprint.Footprint.Height
                << ",\"row_pitch\":" << target.cornerFootprint.Footprint.RowPitch << "}"
                << ",\"queue_type\":" << int(target.type) << ",\"gpu_complete\":" << (complete ? "true" : "false")
                << ",\"frames\":" << frames << ",\"motion\":" << motion << ",\"depth\":" << depth << ",\"hudless\":" << hudless
                << ",\"constants\":" << constants << "}\n";
        }
        { std::lock_guard lock(gFolderMutex); gLastFolder = folder.wstring(); }
        Log("capture_written", "\"folder\":\""+Narrow(folder.wstring())+"\",\"frames\":"+
            std::to_string(written)+",\"slots_used\":"+std::to_string(used)+",\"gpu_complete\":"+(complete ? "true" : "false"));
    }
    catch (...) { Log("capture_failed", "\"reason\":\"write_exception\""); }
}

void WriterThread() noexcept
{
    bool beeped{};
    for (;;)
    {
        WaitForSingleObject(gWake, 1000);
        const int state = gState.load(std::memory_order_acquire);
        if (state == eCapturing && !beeped) { beeped = true; MessageBeep(MB_ICONASTERISK); }
        if (state == ePreparing)
        {
            if (Allocate())
            {
                gCountdown = kCountdownBatches; gBatch = 0;
                gState.store(eCountdown, std::memory_order_release);
            }
            else
            {
                Log("capture_failed", "\"reason\":\"allocation\"");
                ReleaseResources();
                gState.store(eIdle, std::memory_order_release);
            }
        }
        else if (state == eWriting)
        {
            Write();
            beeped = false;
            // The slots are released only after their copies completed.
            if (gFence && gFence->GetCompletedValue() < gSignaled.load())
                Log("capture_failed", "\"reason\":\"gpu_incomplete\"");
            ReleaseResources();
            gSession.clear();
            gState.store(eIdle, std::memory_order_release);
            MessageBeep(MB_OK);
        }
    }
}

void Request() noexcept
{
    int expected = eIdle;
    if (gState.compare_exchange_strong(expected, eRequested))
        Log("capture_requested", "\"batches\":"+std::to_string(kBatches));
}

void HotkeyThread() noexcept
{
    bool down{};
    for (;;)
    {
        Sleep(50);
        const bool now = (GetAsyncKeyState(VK_CONTROL) & 0x8000) && (GetAsyncKeyState(VK_SHIFT) & 0x8000)
            && (GetAsyncKeyState(VK_F9) & 0x8000);
        if (now && !down) Request();
        down = now;
    }
}

const sdk_pacing::PresentCaptureHooks kHooks{&BeforeOutput, &BeforePresent1};

void StartThreads(bool hotkey) noexcept
{
    gWake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!gWake) return;
    sdk_pacing::SetPresentCapture(&kHooks);
    std::thread(WriterThread).detach();
    if (hotkey) std::thread(HotkeyThread).detach();
}

bool Footprint(ID3D12Device* device, DXGI_FORMAT format, UINT width, UINT height,
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT& footprint, UINT64& bytes) noexcept
{
    D3D12_RESOURCE_DESC crop{};
    crop.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    crop.Width = width; crop.Height = height; crop.DepthOrArraySize = 1; crop.MipLevels = 1;
    crop.Format = format; crop.SampleDesc.Count = 1;
    bytes = 0;
    device->GetCopyableFootprints(&crop, 0, 1, 0, &footprint, nullptr, nullptr, &bytes);
    return bytes && bytes <= 64ull*1024*1024;
}

// The first Present after a request: the SDK's queue and its back buffers' format.
void PrepareTarget(IDXGISwapChain1* swapchain) noexcept
{
    const auto current = tCurrent;
    if (!current.valid || !current.queue || !swapchain) return;
    Target target;
    DXGI_SWAP_CHAIN_DESC1 desc{};
    if (FAILED(current.queue->GetDevice(IID_PPV_ARGS(&target.device))) || FAILED(swapchain->GetDesc1(&desc))) return;
    target.queue = current.queue;
    target.type = current.queue->GetDesc().Type;
    target.format = desc.Format; target.width = desc.Width; target.height = desc.Height;
    const UINT width = (std::min)(kCropWidth, desc.Width), height = (std::min)(kCropHeight, desc.Height);
    target.box = {(desc.Width-width)/2, (desc.Height-height)/2, 0, (desc.Width-width)/2+width, (desc.Height-height)/2+height, 1};
    const UINT cornerWidth = (std::min)(kCornerWidth, desc.Width), cornerHeight = (std::min)(kCornerHeight, desc.Height);
    target.cornerBox = {0, 0, 0, cornerWidth, cornerHeight, 1};
    if (!Footprint(target.device.Get(), desc.Format, width, height, target.footprint, target.bytes)
        || !Footprint(target.device.Get(), desc.Format, cornerWidth, cornerHeight, target.cornerFootprint, target.cornerBytes)) return;
    {
        std::lock_guard lock(gTargetMutex);
        if (gState.load(std::memory_order_acquire) != eRequested) return;
        gTarget = target;
        gState.store(ePreparing, std::memory_order_release);
    }
    Log("capture_target", "\"format\":"+std::to_string(int(desc.Format))+",\"width\":"+std::to_string(desc.Width)+
        ",\"height\":"+std::to_string(desc.Height)+",\"crop_x\":"+std::to_string(target.box.left)+",\"crop_y\":"+
        std::to_string(target.box.top)+",\"crop_w\":"+std::to_string(width)+",\"crop_h\":"+std::to_string(height)+
        ",\"corner_w\":"+std::to_string(cornerWidth)+",\"corner_h\":"+std::to_string(cornerHeight)+
        ",\"queue_type\":"+std::to_string(int(target.type))+",\"buffers\":"+std::to_string(desc.BufferCount));
    SetEvent(gWake);
}

void CopyInput(InputKind& kind, ID3D12CommandList* list, uint32_t presentId, const xefg_swapchain_d3d12_resource_data_t& data)
{
    const int state = gState.load(std::memory_order_acquire);
    if (state == eRequested || state == ePreparing)
    {
        std::lock_guard lock(gInputMutex);
        kind.desc = data.pResource->GetDesc(); kind.state = data.incomingState; kind.known = true;
        return;
    }
    if (state != eCapturing) return;
    D3D12_RESOURCE_DESC expected{};
    size_t count{};
    { std::lock_guard lock(gInputMutex); if (!kind.known) return; expected = kind.desc; count = kind.copies.size(); }
    if (kind.used.load() >= count) return;
    const auto desc = data.pResource->GetDesc();
    if (desc.Format != expected.Format || desc.Width != expected.Width || desc.Height != expected.Height
        || desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || desc.SampleDesc.Count != 1) return;
    ComPtr<ID3D12GraphicsCommandList> graphics;
    if (FAILED(list->QueryInterface(IID_PPV_ARGS(&graphics))) || !graphics) return;
    const uint32_t i = kind.used.fetch_add(1);
    if (i >= count) return;
    auto& copy = kind.copies[i];
    if (!copy.buffer) return;
    const bool transition = data.incomingState != D3D12_RESOURCE_STATE_COPY_SOURCE;
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = data.pResource;
    barrier.Transition.Subresource = 0;
    barrier.Transition.StateBefore = data.incomingState;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    if (transition) graphics->ResourceBarrier(1, &barrier);
    D3D12_TEXTURE_COPY_LOCATION destination{};
    destination.pResource = copy.buffer.Get();
    destination.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    destination.PlacedFootprint = copy.footprint;
    D3D12_TEXTURE_COPY_LOCATION source{};
    source.pResource = data.pResource;
    source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    source.SubresourceIndex = 0;
    graphics->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
    if (transition)
    {
        std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
        graphics->ResourceBarrier(1, &barrier);
    }
    copy.presentId = presentId;
    copy.recorded = true;
}
}

std::string ReadStartupOption(const std::wstring& path, const char* key)
{
    std::ifstream file{std::filesystem::path(path)};
    std::string line, found;
    while (file && std::getline(file, line))
    {
        const auto equals = line.find('=');
        if (equals == std::string::npos) continue;
        std::string name = line.substr(0, equals), value = line.substr(equals+1);
        const auto trim = [](std::string& s) {
            while (!s.empty() && (s.back() == '\r' || s.back() == ' ' || s.back() == '\t')) s.pop_back();
            while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.erase(s.begin());
        };
        trim(name); trim(value);
        if (name == key) found = value; // the last line wins; "# key" lines are other names
    }
    return found;
}

std::string StartupOption(const char* key) noexcept
{
    try
    {
        const auto marker = ModuleDirectoryFile(L"RTXMFG-capture.enable");
        return marker.empty() ? std::string() : ReadStartupOption(marker, key);
    }
    catch (...) { return {}; }
}

bool Enabled() noexcept
{
    char value[8]{};
    const DWORD length = GetEnvironmentVariableA("RTXMFG_XESS_CAPTURE", value, DWORD(sizeof(value)));
    const auto marker = ModuleDirectoryFile(L"RTXMFG-capture.enable");
    return (length == 1 && value[0] == '1') || (!marker.empty() && GetFileAttributesW(marker.c_str()) != INVALID_FILE_ATTRIBUTES);
}

void Start() noexcept
{
    std::call_once(gStartOnce, [] {
        if (!Enabled()) return;
        gEnabled = true;
        StartThreads(true);
        Log("capture_armed", "\"hotkey\":\"Ctrl+Shift+F9\",\"batches\":"+std::to_string(kBatches)+
            ",\"crop\":\""+std::to_string(kCropWidth)+"x"+std::to_string(kCropHeight)+"\",\"input_fixes\":"+InputFixes());
    });
}

void OnFrameConstants(uint32_t presentId, const xefg_swapchain_frame_constant_data_t& data) noexcept
{
    const int state = gState.load(std::memory_order_acquire);
    if (!gEnabled || (state != eCountdown && state != eCapturing)) return;
    try
    {
        std::lock_guard lock(gConstantsMutex);
        if (gConstants.size() < kMaxConstants) gConstants.emplace_back(presentId, data);
    }
    catch (...) {}
}

void OnTagResource(ID3D12CommandList* list, uint32_t presentId, const xefg_swapchain_d3d12_resource_data_t& data) noexcept
{
    if (!gEnabled || !data.pResource || !list) return;
    try
    {
        if (data.type == XEFG_SWAPCHAIN_RES_MOTION_VECTOR) CopyInput(gMotion, list, presentId, data);
        else if (data.type == XEFG_SWAPCHAIN_RES_DEPTH) CopyInput(gDepth, list, presentId, data);
        else if (data.type == XEFG_SWAPCHAIN_RES_HUDLESS_COLOR) CopyInput(gHudless, list, presentId, data);
    }
    catch (...) {}
}

void BeforeOutput(OutputKind kind, uint32_t presentId, ID3D12CommandQueue* queue) noexcept
{
    tCurrent = {true, kind, presentId, queue};
    if (kind == OutputKind::eFirst) gIndexInBatch = 0;
    else gIndexInBatch.fetch_add(1, std::memory_order_relaxed);
    if (kind != OutputKind::eFirst) return;
    const int state = gState.load(std::memory_order_acquire);
    if (state == eCountdown && gCountdown.fetch_sub(1) == 1)
    {
        gBatch = 0;
        gState.store(eCapturing, std::memory_order_release);
        SetEvent(gWake); // start beep
        Log("capture_start", "\"present_id\":"+std::to_string(presentId));
    }
    else if (state == eCapturing && (gBatch.fetch_add(1)+1 >= kBatches || gUsed.load() >= kSlots))
    {
        gState.store(eWriting, std::memory_order_release);
        SetEvent(gWake);
    }
}

void BeforePresent1(IDXGISwapChain1* swapchain) noexcept
{
    const int state = gState.load(std::memory_order_acquire);
    if (state == eRequested) { PrepareTarget(swapchain); return; }
    if (state != eCapturing) return;
    const auto current = tCurrent;
    if (!current.valid || current.kind == OutputKind::eOther || !swapchain) return;
    try
    {
        const Target& target = gTarget;
        if (current.queue != target.queue.Get()) return;
        const uint32_t slot = gUsed.fetch_add(1);
        if (slot >= kSlots || slot >= gSlots.size()) return;
        auto& meta = gMeta[slot];
        meta = {};
        ComPtr<IDXGISwapChain3> chain;
        ComPtr<ID3D12Resource> back;
        if (FAILED(swapchain->QueryInterface(IID_PPV_ARGS(&chain))) || !chain) return;
        const UINT index = chain->GetCurrentBackBufferIndex();
        if (FAILED(chain->GetBuffer(index, IID_PPV_ARGS(&back))) || !back) return;
        const auto desc = back->GetDesc();
        if (desc.Width != target.width || desc.Height != target.height || desc.Format != target.format) return;
        auto& recorder = gSlots[slot];
        if (FAILED(recorder.allocator->Reset()) || FAILED(recorder.list->Reset(recorder.allocator.Get(), nullptr))) return;
        D3D12_TEXTURE_COPY_LOCATION source{};
        source.pResource = back.Get();
        source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        source.SubresourceIndex = 0;
        D3D12_TEXTURE_COPY_LOCATION destination{};
        destination.pResource = recorder.buffer.Get();
        destination.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        destination.PlacedFootprint = target.footprint;
        recorder.list->CopyTextureRegion(&destination, 0, 0, 0, &source, &target.box);
        destination.pResource = recorder.corner.Get();
        destination.PlacedFootprint = target.cornerFootprint;
        recorder.list->CopyTextureRegion(&destination, 0, 0, 0, &source, &target.cornerBox);
        if (FAILED(recorder.list->Close())) return;
        ID3D12CommandList* lists[] = {recorder.list.Get()};
        target.queue->ExecuteCommandLists(1, lists);
        const uint64_t value = gFenceBase.load()+slot+1;
        target.queue->Signal(gFence.Get(), value);
        for (uint64_t seen = gSignaled.load(); seen < value && !gSignaled.compare_exchange_weak(seen, value);) {}
        meta.kind = int(current.kind); meta.presentId = current.presentId;
        meta.index = gIndexInBatch.load(std::memory_order_relaxed); meta.batch = gBatch.load(std::memory_order_relaxed);
        meta.backBuffer = index; meta.ns = NowNs();
        meta.valid = true;
    }
    catch (...) {}
}

#if defined(RTXMFG_CAPTURE_TESTING)
void TestEnable(const wchar_t* root) noexcept
{
    std::call_once(gStartOnce, [&] { gRoot = root ? root : L""; gEnabled = true; StartThreads(false); });
}
void TestRequest() noexcept { Request(); }
int TestState() noexcept { return gState.load(); }
bool TestWaitState(int state, DWORD milliseconds) noexcept
{
    for (DWORD waited = 0; waited <= milliseconds; waited += 10)
    {
        if (gState.load() == state) return true;
        Sleep(10);
    }
    return false;
}
std::wstring TestLastFolder() noexcept { std::lock_guard lock(gFolderMutex); return gLastFolder; }
std::string TestStartupOption(const wchar_t* path, const char* key) noexcept
{
    try { return ReadStartupOption(path, key); } catch (...) { return {}; }
}
#endif
}
