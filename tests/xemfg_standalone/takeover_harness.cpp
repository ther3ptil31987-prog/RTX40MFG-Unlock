// xemfgsa.1: XeMFG standalone's D3D12 takeover through MFG Unlocker loaded as winmm.dll (the release-regression
// convention). The runner puts the profiled XeSS SDK 3.0.2 pair in RTXMFG-XeSS beside it.
//   hardware  XeFG on the first hardware adapter, fed depth and motion vectors by the stand-in libxess
//             (XessFixture.dll): the chain is XeFG's, frames generate at 4x, a released chain goes and a new one
//             is taken over again.
//   warp      XeFG refuses WARP: the chain is created natively and presents.
//   off       switch off: native chain; switched on afterwards, the status asks for a restart.
//   limit     xemfgsa.5: hardware with the menu's FPS limit at 60: XeLL holds XeFG's presented frames to it (the
//             game's at ~15 at 4x; the status's frame rates and xemfgFpsLimited).
//   dynamic   xemfgsa.6: hardware with Dynamic MFG up to 6X and a 120 FPS target: the test scene runs far above it at
//             6X, so the count steps down (the status's xemfgDynamicActive and xemfgFrames).
#include "common.h"
#include <chrono>
#include <fstream>
#include <sstream>
#include <thread>

using namespace harness;

namespace
{
std::filesystem::path Here()
{
    wchar_t path[MAX_PATH]{};
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    return std::filesystem::path(path).parent_path();
}

std::string ReadText(const std::filesystem::path& path)
{
    std::ifstream file(path, std::ios::binary);
    std::stringstream text;
    text << file.rdbuf();
    return text.str();
}

bool WaitForStatus(const char* needle, unsigned milliseconds)
{
    const auto status = Here() / L"RTXMFG-Universal.status.json";
    for (unsigned waited = 0; waited <= milliseconds; waited += 50) {
        if (ReadText(status).find(needle) != std::string::npos) return true;
        Sleep(50);
    }
    std::printf("NOTE status: %s\n", ReadText(status).substr(0, 4000).c_str());
    return false;
}

// The status's integer field `key` (0 when absent).
uint64_t StatusNumber(const char* key)
{
    const std::string text = ReadText(Here() / L"RTXMFG-Universal.status.json");
    const std::string needle = std::string("\"") + key + "\":";
    const size_t at = text.find(needle);
    return at == std::string::npos ? 0 : std::strtoull(text.c_str() + at + needle.size(), nullptr, 10);
}

bool WriteControl(bool standalone, uint32_t frames, uint32_t limitFps = 0, bool dynamic = false, uint32_t targetFps = 0)
{
    const auto path = Here() / L"RTXMFG-Universal.json";
    const auto temporary = Here() / L"RTXMFG-Universal.json.tmp";
    {
        std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
        file << "{\"followGame\":true,\"mode\":\"follow\",\"multiplier\":2,\"dynamicTargetFrameRate\":0,\"dlssgPreset\":0,"
                "\"vsyncMode\":0,\"reflexFrameLimitFps\":" << limitFps << ",\"generatedOnlyDebug\":false,\"intervalLogging\":false,"
                "\"version\":13,\"xemfgStandalone\":" << (standalone ? 1 : 0) << ",\"xemfgFrames\":" << frames
             << ",\"xemfgDynamic\":" << (dynamic ? 1 : 0) << ",\"xemfgTargetFps\":" << targetFps << "}\n";
        if (!file) return false;
    }
    return MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != FALSE;
}

// libxess's xess_d3d12_execute_params_t (as the host mirrors it).
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
using ExecuteFn = int32_t(*)(void*, ID3D12GraphicsCommandList*, const XessExecuteParams*);

ComPtr<ID3D12Resource> Texture(ID3D12Device* device, DXGI_FORMAT format, UINT width, UINT height, D3D12_RESOURCE_STATES state)
{
    D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_DEFAULT};
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = width;
    desc.Height = height;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = format;
    desc.SampleDesc.Count = 1;
    ComPtr<ID3D12Resource> resource;
    if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, state, nullptr, IID_PPV_ARGS(&resource))))
        resource.Reset();
    return resource;
}

struct Chain
{
    ComPtr<IDXGISwapChain3> swapchain;
    ComPtr<ID3D12DescriptorHeap> heap;
    std::vector<ComPtr<ID3D12Resource>> buffers;
    UINT rtvSize = 0;
};

bool CreateChain(WarpFixture& fixture, HWND window, Chain& chain)
{
    DXGI_SWAP_CHAIN_DESC1 desc{640, 480, DXGI_FORMAT_B8G8R8A8_UNORM, FALSE, {1, 0}, DXGI_USAGE_RENDER_TARGET_OUTPUT, 3,
                               DXGI_SCALING_STRETCH, DXGI_SWAP_EFFECT_FLIP_DISCARD, DXGI_ALPHA_MODE_UNSPECIFIED, 0};
    ComPtr<IDXGISwapChain1> created;
    if (FAILED(fixture.factory->CreateSwapChainForHwnd(fixture.queue.Get(), window, &desc, nullptr, nullptr, &created)) || !created ||
        FAILED(created.As(&chain.swapchain)))
        return false;
    D3D12_DESCRIPTOR_HEAP_DESC heap{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 3};
    if (FAILED(fixture.device->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&chain.heap)))) return false;
    chain.rtvSize = fixture.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    for (UINT i = 0; i < 3; ++i) {
        ComPtr<ID3D12Resource> buffer;
        if (FAILED(chain.swapchain->GetBuffer(i, IID_PPV_ARGS(&buffer)))) return false;
        auto handle = chain.heap->GetCPUDescriptorHandleForHeapStart();
        handle.ptr += SIZE_T(i) * chain.rtvSize;
        fixture.device->CreateRenderTargetView(buffer.Get(), nullptr, handle);
        chain.buffers.push_back(buffer);
    }
    return true;
}

// One base frame: the stand-in upscaler on the frame's list (the host tags its inputs there), a clear, the present.
bool Frame(WarpFixture& fixture, Chain& chain, ExecuteFn execute, XessExecuteParams* params, unsigned index, HRESULT& presented)
{
    if (!fixture.SubmitAndWait()) return false;
    if (execute) execute(reinterpret_cast<void*>(uintptr_t(0x1000)), fixture.commands.Get(), params);
    const UINT back = chain.swapchain->GetCurrentBackBufferIndex();
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = chain.buffers[back].Get();
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    fixture.commands->ResourceBarrier(1, &barrier);
    auto rtv = chain.heap->GetCPUDescriptorHandleForHeapStart();
    rtv.ptr += SIZE_T(back) * chain.rtvSize;
    const float shade = float(index % 60) / 60.0f;
    const float color[4] = {shade, 0.25f, 1.0f - shade, 1.0f};
    fixture.commands->ClearRenderTargetView(rtv, color, 0, nullptr);
    std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
    fixture.commands->ResourceBarrier(1, &barrier);
    if (FAILED(fixture.commands->Close())) return false;
    ID3D12CommandList* lists[] = {fixture.commands.Get()};
    fixture.queue->ExecuteCommandLists(1, lists);
    presented = chain.swapchain->Present(0, 0);
    return fixture.SignalAndWait();
}

int Run(const std::wstring& mode, const wchar_t* dll)
{
    const bool limit = mode == L"limit", dynamic = mode == L"dynamic";
    const bool hardware = mode == L"hardware" || limit || dynamic, off = mode == L"off";
    if (!Check(dynamic ? WriteControl(true, 6, 0, true, 120) : WriteControl(!off, 4, limit ? 60 : 0), "control file written"))
        return 1;
    HMODULE module = LoadCandidate(dll);
    if (!Check(module != nullptr, "candidate loaded")) return 1;
    if (!Check(WaitForStatus(off ? "\"xemfgState\":\"off\"" : "\"xemfgState\":\"waiting\"", 20000), "XeMFG standalone state published"))
        return 1;
    // The stand-in upscaler loads after MFG Unlocker, as a game's upscaler DLL does: the load notification has the
    // worker hook it.
    ExecuteFn execute = nullptr;
    HMODULE fixtureModule = nullptr;
    if (hardware) {
        fixtureModule = LoadLibraryW((Here() / L"XessFixture.dll").c_str());
        execute = fixtureModule ? reinterpret_cast<ExecuteFn>(GetProcAddress(fixtureModule, "xessD3D12Execute")) : nullptr;
        if (!Check(execute != nullptr, "stand-in libxess loaded")) return 1;
        Sleep(1500);
    }
    ComPtr<IDXGIAdapter> adapter;
    if (hardware) {
        ComPtr<IDXGIFactory1> factory;
        if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) return Check(false, "adapter factory"), 1;
        for (UINT i = 0;; ++i) {
            ComPtr<IDXGIAdapter1> candidate;
            if (factory->EnumAdapters1(i, &candidate) == DXGI_ERROR_NOT_FOUND) break;
            DXGI_ADAPTER_DESC1 desc{};
            if (SUCCEEDED(candidate->GetDesc1(&desc)) && !(desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) {
                adapter = candidate;
                std::printf("NOTE adapter %ls\n", desc.Description);
                break;
            }
        }
        if (!Check(adapter != nullptr, "hardware adapter found")) return 1;
    }
    WarpFixture fixture;
    if (!Check(fixture.Initialize(false, adapter.Get()), hardware ? "hardware device, queue and factory created"
                                                                   : "WARP device, queue and factory created"))
        return 1;
    TestWindow window(L"XemfgStandalone", 640, 480);
    if (!Check(window.window != nullptr, "test window created")) return 1;
    window.Show();
    window.Pump(4);

    ComPtr<ID3D12Resource> depth = Texture(fixture.device.Get(), DXGI_FORMAT_R32_FLOAT, 640, 480, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    ComPtr<ID3D12Resource> motion = Texture(fixture.device.Get(), DXGI_FORMAT_R16G16_FLOAT, 640, 480, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    if (!Check(depth && motion, "upscaler inputs created")) return 1;
    XessExecuteParams params{};
    params.depth = depth.Get();
    params.velocity = motion.Get();
    params.inputWidth = 640;
    params.inputHeight = 480;

    const unsigned rounds = hardware && !limit && !dynamic ? 2 : 1;
    for (unsigned round = 0; round < rounds; ++round) {
        const auto wrappersBefore = Status(module).swapchainWrappersCreated;
        Chain chain;
        if (!Check(CreateChain(fixture, window.window, chain), round ? "chain created again after release" : "chain created")) return 1;
        // The game's chain gets a proxy (a menu-less one under XeMFG, which puts the menu on XeFG's output chain).
        Check(Status(module).swapchainWrappersCreated >= wrappersBefore + 1, "a proxy wraps the chain");
        if (off) {
            Check(WriteControl(true, 4), "switched on after the chain");
            Check(WaitForStatus("\"xemfgState\":\"restart\"", 10000), "status asks for a restart");
        }
        const unsigned frames = limit ? 150 : dynamic ? 2400 : hardware ? (round ? 120 : 700) : 120;
        unsigned failedPresents = 0;
        for (unsigned i = 0; i < frames; ++i) {
            HRESULT presented = S_OK;
            if (!Frame(fixture, chain, execute, &params, i, presented)) return Check(false, "frame submitted"), 1;
            if (FAILED(presented)) ++failedPresents;
            if ((i & 15) == 0) window.Pump(1);
        }
        Check(failedPresents == 0, "every present succeeded");
        if (dynamic) {
            // xemfgsa.6: Dynamic MFG ran and stepped down from 6X toward the 120 FPS target.
            Check(WaitForStatus("\"xemfgDynamicActive\":true", 5000), "Dynamic MFG runs");
            Check(WaitForStatus("\"xemfgDynamicTargetMilli\":120000", 3000), "its target is the one set");
            const uint64_t shown = StatusNumber("xemfgFrames");
            const uint64_t base = StatusNumber("xemfgBaseFpsMilli"), output = StatusNumber("xemfgOutputFpsMilli");
            std::printf("NOTE dynamic: %llux, frame rates base %.1f output %.1f\n", static_cast<unsigned long long>(shown),
                base / 1000.0, output / 1000.0);
            Check(shown >= 1 && shown < 6, "the count stepped down from 6X");
        } else if (hardware) {
            Check(WaitForStatus("\"xemfgFrames\":4", 5000), "the chain is XeFG's at 4x");
            Check(WaitForStatus("\"xemfgSource\":\"xess\"", 5000), "inputs from the stand-in XeSS upscaler");
            Check(WaitForStatus("\"xemfgGenerating\":true", 5000), "XeFG generates");
            // xemfgsa.5: the frame rates for the menu's FPS row (XeFG's presents per game frame = 4).
            Check(WaitForStatus("\"xemfgBaseFpsMilli\":", 3000), "frame rates published");
            const uint64_t base = StatusNumber("xemfgBaseFpsMilli"), output = StatusNumber("xemfgOutputFpsMilli");
            std::printf("NOTE frame rates: base %.1f output %.1f\n", base / 1000.0, output / 1000.0);
            Check(base > 0 && output >= base * 3 && output <= base * 5, "output about 4x the game's frame rate");
            if (limit) {
                Check(WaitForStatus("\"xemfgFpsLimited\":true", 3000), "XeLL holds the FPS limit");
                Check(output >= 50000 && output <= 66000, "XeMFG presents about the 60 FPS limit");
            }
        } else if (!off) {
            Check(WaitForStatus("\"xemfgFrames\":0", 5000), "WARP keeps the game's own chain");
        }
        chain = Chain{};
        Check(fixture.SignalAndWait(), "queue idle after release");
        Sleep(300);
    }
    std::printf("COMPLETE mode=%ls failures=%d\n", mode.c_str(), gFailures);
    return gFailures ? 10 : 0;
}
}

int wmain(int argc, wchar_t** argv)
{
    if (argc < 3) { std::printf("usage: XemfgTakeoverHarness hardware|warp|off <candidate dll>\n"); return 2; }
    std::printf("mode=%ls dll=%ls\n", argv[1], argv[2]);
    return Run(argv[1], argv[2]);
}
