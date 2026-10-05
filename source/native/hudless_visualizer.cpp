#include "hudless_visualizer.h"
#include "hudless_probe.h"

#include <Windows.h>
#include <d3d12.h>
#include <d3dcompiler.h>
#include <wrl/client.h>

#include <array>
#include <atomic>
#include <cstring>
#include <mutex>
#include <vector>

namespace hudless_visualizer
{
namespace
{
using Microsoft::WRL::ComPtr;

constexpr uint32_t kRing = 8; // Submissions in flight before a Present's pass is skipped.
// Engines may tag HUDless frames ahead of their Present, so tag-time copies are
// kept per frame. With frame indices and present markers the copy tagged for the
// presented frame is used; without them the tint compares against every kept
// copy and synthesis uses the latest.
constexpr uint32_t kTagSlots = 4;
constexpr uint32_t kCandidates = kTagSlots;
constexpr uint32_t kSrvPerDraw = 1 + kCandidates;
constexpr uint32_t kAlphaRing = 3;
constexpr uint32_t kOnlyValidNow = 0;
// Differences below about three 8-bit steps are treated as equal.
constexpr float kThreshold = 0.012f;
constexpr float kStrength = 0.6f;
// Synthesized alpha is tagged in the state DLSS-G reads it in.
constexpr UINT kShaderRead = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE
    | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
constexpr uint64_t kHealthyMs = 1000;
// Learned-alpha state per pixel: four halves (see synth).
constexpr uint64_t kStateBytesPerPixel = 8;

enum class Status : uint32_t { eOff, eWaiting, eDrawing, eShapeMismatch, eUnsupported, eFailed };
enum class SynthStatus : uint32_t
{
    eOff, eWaiting, eTagging, eReused, eShapeMismatch, eUnsupported, eFailed, eRejected, eNoTagger
};

std::atomic<bool> gEnabled{false};
std::atomic<bool> gSynthesisActive{false};
std::atomic<UiTagger> gTagger{nullptr};
std::atomic<uint32_t> gStatus{static_cast<uint32_t>(Status::eOff)};
std::atomic<uint32_t> gSynthStatus{static_cast<uint32_t>(SynthStatus::eOff)};
std::atomic<uint64_t> gFramesDrawn{0};
std::atomic<uint64_t> gPairedFrames{0};
std::atomic<uint64_t> gSynthFrames{0};
std::atomic<uint64_t> gSynthReused{0};
std::atomic<uint64_t> gSynthPrimes{0};
enum class AlphaMode : uint32_t { eNone, eLearned, ePerFrame };
std::atomic<uint32_t> gSynthAlphaMode{static_cast<uint32_t>(AlphaMode::eNone)};
std::atomic<bool> gRelearn{false}; // Set when synthesis starts; the next pass primes.
std::atomic<uint64_t> gLastFreshTagTick{0};
std::mutex gMutex;

struct Latest
{
    ComPtr<ID3D12Resource> resource; // Present-time source for later lifecycles.
    UINT state = 0;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    uint32_t left = 0;
    uint32_t top = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t frame = hudless_probe::kNoFrame;
    bool copiedAtTag = false;
    uint64_t sequence = 0;
};
Latest gLatest;
uint64_t gDrawnSequence = 0;  // Present-time HUDless the tint last read.
uint64_t gSynthSequence = 0;  // Present-time HUDless synthesis last read.
bool gBroken = false; // A submission could not be fenced; its objects stay retired.

struct TagCopy
{
    std::array<ComPtr<ID3D12Resource>, kTagSlots> textures;
    std::array<uint64_t, kTagSlots> sequences{}; // Tag sequence copied into each slot; 0 = empty.
    std::array<uint32_t, kTagSlots> frames{};    // Streamline frame index of each slot.
    uint32_t next = 0;
    ComPtr<ID3D12Device> device;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    uint32_t width = 0;
    uint32_t height = 0;
};
TagCopy gTagCopy;

struct Pipeline
{
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12RootSignature> root;
    ComPtr<ID3D12PipelineState> pso;
    DXGI_FORMAT rtvFormat = DXGI_FORMAT_UNKNOWN;
    ComPtr<ID3D12PipelineState> synthPso;
    ComPtr<ID3D12DescriptorHeap> srvHeap;
    ComPtr<ID3D12DescriptorHeap> rtvHeap; // Backbuffer, then the alpha ring.
    UINT srvStep = 0;
    UINT rtvStep = 0;
    std::array<ComPtr<ID3D12CommandAllocator>, kRing> allocators;
    std::array<UINT64, kRing> fenceValues{};
    std::array<ComPtr<ID3D12Resource>, kRing> heldSources; // Game resources read by a slot.
    ComPtr<ID3D12GraphicsCommandList> list;
    ComPtr<ID3D12Fence> fence;
    UINT64 nextFence = 1;
    uint32_t slot = 0;
    ComPtr<ID3D12Resource> scratch;
    DXGI_FORMAT scratchFormat = DXGI_FORMAT_UNKNOWN;
    uint32_t scratchWidth = 0;
    uint32_t scratchHeight = 0;
    ComPtr<ID3D12Resource> presentCopy;
    DXGI_FORMAT presentCopyFormat = DXGI_FORMAT_UNKNOWN;
    uint32_t presentCopyWidth = 0;
    uint32_t presentCopyHeight = 0;
    std::array<ComPtr<ID3D12Resource>, kAlphaRing> alpha;
    uint32_t alphaWidth = 0;
    uint32_t alphaHeight = 0;
    uint32_t alphaNext = 0;
    int32_t alphaLast = -1; // Ring index of the last freshly derived alpha.
    ComPtr<ID3D12PipelineState> synthBoundPso; // Per-frame alpha when there is no state.
    // Learned-alpha state, in COMMON between passes. Null with a nonzero size
    // means allocation failed for that size and is not retried.
    ComPtr<ID3D12Resource> state;
    uint32_t stateWidth = 0;
    uint32_t stateHeight = 0;
    bool statePrimed = false; // Holds a previous frame; false primes on the next pass.
};
Pipeline gPipe;
ComPtr<ID3DBlob> gVertexShader;
ComPtr<ID3DBlob> gPixelShader;
ComPtr<ID3DBlob> gSynthShader;
ComPtr<ID3DBlob> gSynthBoundShader;
// Objects a submitted command list may still reference are never released.
std::vector<ComPtr<ID3D12DeviceChild>> gRetired;

constexpr char kVertexShader[] =
    "float4 main(uint id : SV_VertexID) : SV_Position\n"
    "{\n"
    "    float2 uv = float2((id << 1) & 2, id & 2);\n"
    "    return float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);\n"
    "}\n";

// Relation values follow hudless_detection::Hypothesis. Count is the number of
// HUDless candidates bound (1 to kCandidates). Width and Prime are for synth.
constexpr char kPixelShader[] =
    "Texture2D<float4> FinalColor : register(t0);\n"
    "Texture2D<float4> Hudless[4] : register(t1);\n"
    "RWByteAddressBuffer SynthState : register(u1);\n"
    "cbuffer Constants : register(b0)\n"
    "{\n"
    "    float Threshold; float Strength; uint Relation; uint Count;\n"
    "    uint Width; uint Prime; uint2 Unused;\n"
    "};\n"
    "float3 SrgbEncode(float3 v) { v = saturate(v); return v <= 0.0031308 ? v * 12.92 : 1.055 * pow(v, 1.0 / 2.4) - 0.055; }\n"
    "float3 SrgbDecode(float3 v) { v = saturate(v); return v <= 0.04045 ? v / 12.92 : pow((v + 0.055) / 1.055, 2.4); }\n"
    "float3 Pq(float3 nits)\n"
    "{\n"
    "    float3 y = pow(saturate(nits / 10000.0), 2610.0 / 16384.0);\n"
    "    return pow((3424.0 / 4096.0 + 2413.0 / 128.0 * y) / (1.0 + 2392.0 / 128.0 * y), 2523.0 / 32.0);\n"
    "}\n"
    "float3 Relate(float3 v)\n"
    "{\n"
    "    if (Relation == 1) return SrgbEncode(v);\n"
    "    if (Relation == 2) return pow(saturate(v), 1.0 / 2.2);\n"
    "    if (Relation == 3) return SrgbDecode(v);\n"
    "    if (Relation == 4)\n"
    "    {\n"
    "        float3x3 toBt2020 = float3x3(0.6274040, 0.3292820, 0.0433136,\n"
    "            0.0690970, 0.9195400, 0.0113612, 0.0163916, 0.0880132, 0.8955950);\n"
    "        return Pq(mul(toBt2020, v) * 80.0);\n"
    "    }\n"
    "    return v;\n"
    "}\n"
    "float Difference(float3 f, float3 h) { float3 d = abs(f - Relate(h)); return max(max(d.r, d.g), d.b); }\n"
    "float4 main(float4 position : SV_Position) : SV_Target\n"
    "{\n"
    "    int3 p = int3(int2(position.xy), 0);\n"
    "    float3 f = FinalColor.Load(p).rgb;\n"
    "    float d = Difference(f, Hudless[0].Load(p).rgb);\n"
    "    [unroll] for (uint i = 1; i < 4; ++i)\n"
    "        if (i < Count) d = min(d, Difference(f, Hudless[i].Load(p).rgb));\n"
    "    float m = saturate((d - Threshold) * 32.0);\n"
    "    return float4(0.9, 0.05, 0.9, m * Strength);\n"
    "}\n"
    // The smallest alpha explaining final = ui + (1 - alpha) * hudless with a
    // premultiplied UI colour in [0, alpha]: exact for black or white UI of any
    // opacity, a lower bound for coloured translucent UI.
    "float LowerBound(float3 f, float3 h)\n"
    "{\n"
    "    float3 d = f - h;\n"
    "    if (max(max(abs(d.r), abs(d.g)), abs(d.b)) <= Threshold) return 0.0;\n"
    "    float3 brighter = d / max(1.0 - h, 1.0e-4);\n"
    "    float3 darker = -d / max(h, 1.0e-4);\n"
    "    float3 a = d > 0.0 ? brighter : darker;\n"
    "    return saturate(max(max(a.r, a.g), a.b));\n"
    "}\n"
    "float4 synthBound(float4 position : SV_Position) : SV_Target\n"
    "{\n"
    "    int3 p = int3(int2(position.xy), 0);\n"
    "    return LowerBound(FinalColor.Load(p).rgb, Relate(Hudless[0].Load(p).rgb)).xxxx;\n"
    "}\n"
    // One frame cannot tell a translucent grey panel from a fainter one: where
    // its colour matches the scene behind it the bound is 0. While the scene
    // moves behind a still UI, final colour changes by (1 - alpha) times the
    // HUDless change, so the least-squares slope of those changes is the
    // opacity DLSS-G needs to composite the UI over its interpolated scene.
    // State per pixel, four halves: the previous final and HUDless (channel
    // means), then n = sum dF*dH and m = sum dH*dH, decayed in frames where the
    // HUDless changed and held while it is still. A change the slope cannot
    // explain means the UI changed and learning restarts; until the sums are
    // trusted the per-frame bound is used.
    "static const float kStill = 4.0e-6;\n" // HUDless change under about two 10-bit steps.
    "static const float kNoise = 1.6e-5;\n" // Residual allowance, about four 10-bit steps.
    "static const float kResidual = 0.05;\n" // Missing a change by over ~22% restarts learning.
    "static const float kDecay = 0.9;\n"
    "static const float kTrust = 1.0e-3;\n" // Variation needed before the slope is used...
    "static const float kFull = 1.0e-2;\n"  // ...and before it is used alone.
    "static const float kSnap = 0.02;\n"
    "float4 synth(float4 position : SV_Position) : SV_Target\n"
    "{\n"
    "    int3 p = int3(int2(position.xy), 0);\n"
    "    float3 f = FinalColor.Load(p).rgb;\n"
    "    float3 h = Relate(Hudless[0].Load(p).rgb);\n"
    "    float bound = LowerBound(f, h);\n"
    "    uint address = (uint(p.y) * Width + uint(p.x)) * 8;\n"
    "    float fs = (f.r + f.g + f.b) / 3.0;\n"
    "    float hs = (h.r + h.g + h.b) / 3.0;\n"
    "    float n = 0.0;\n"
    "    float m = 0.0;\n"
    "    if (Prime == 0)\n"
    "    {\n"
    "        uint2 last = SynthState.Load2(address);\n"
    "        n = f16tof32(last.y);\n"
    "        m = f16tof32(last.y >> 16);\n"
    "        float df = fs - f16tof32(last.x);\n"
    "        float dh = hs - f16tof32(last.x >> 16);\n"
    "        uint changed = 0;\n"
    "        if (m >= kTrust)\n"
    "        {\n"
    "            float e = df - n / m * dh;\n"
    "            if (e * e > kResidual * max(df * df, dh * dh) + kNoise) changed = 1;\n"
    "        }\n"
    "        if (changed != 0)\n"
    "        {\n"
    "            n = 0.0;\n"
    "            m = 0.0;\n"
    "        }\n"
    "        else if (dh * dh > kStill)\n"
    "        {\n"
    "            n = n * kDecay + df * dh;\n"
    "            m = m * kDecay + dh * dh;\n"
    "        }\n"
    "    }\n"
    "    SynthState.Store2(address, uint2(f32tof16(fs) | (f32tof16(hs) << 16),\n"
    "        f32tof16(n) | (f32tof16(m) << 16)));\n"
    "    if (m < kTrust) return bound.xxxx;\n"
    "    float learned = saturate(1.0 - n / m);\n"
    "    learned = learned < kSnap ? 0.0 : (learned > 1.0 - kSnap ? 1.0 : learned);\n"
    "    return lerp(bound, learned, saturate((m - kTrust) / (kFull - kTrust))).xxxx;\n"
    "}\n";

void SetStatus(Status status) noexcept
{
    gStatus.store(static_cast<uint32_t>(status), std::memory_order_release);
}

void SetSynthStatus(SynthStatus status) noexcept
{
    gSynthStatus.store(static_cast<uint32_t>(status), std::memory_order_release);
}

DXGI_FORMAT TypedFormat(DXGI_FORMAT format) noexcept
{
    switch (format)
    {
    case DXGI_FORMAT_R10G10B10A2_TYPELESS: return DXGI_FORMAT_R10G10B10A2_UNORM;
    case DXGI_FORMAT_R8G8B8A8_TYPELESS: return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_B8G8R8A8_TYPELESS: return DXGI_FORMAT_B8G8R8A8_UNORM;
    case DXGI_FORMAT_B8G8R8X8_TYPELESS: return DXGI_FORMAT_B8G8R8X8_UNORM;
    case DXGI_FORMAT_R16G16B16A16_TYPELESS: return DXGI_FORMAT_R16G16B16A16_FLOAT;
    case DXGI_FORMAT_R32G32B32A32_TYPELESS: return DXGI_FORMAT_R32G32B32A32_FLOAT;
    default: return format;
    }
}

D3D12_RESOURCE_BARRIER Transition(ID3D12Resource* resource, UINT before, UINT after,
    UINT subresource = 0) noexcept
{
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = resource;
    barrier.Transition.Subresource = subresource;
    barrier.Transition.StateBefore = static_cast<D3D12_RESOURCE_STATES>(before);
    barrier.Transition.StateAfter = static_cast<D3D12_RESOURCE_STATES>(after);
    return barrier;
}

template <class T> void Retire(ComPtr<T>& object) noexcept
{
    if (object) gRetired.push_back(std::move(object));
    object.Reset();
}

bool CreateTexture(ID3D12Device* device, DXGI_FORMAT format, uint32_t width, uint32_t height,
    ComPtr<ID3D12Resource>& output, D3D12_RESOURCE_FLAGS flags = D3D12_RESOURCE_FLAG_NONE,
    UINT initialState = D3D12_RESOURCE_STATE_COPY_DEST) noexcept
{
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = width;
    desc.Height = height;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = format;
    desc.SampleDesc.Count = 1;
    desc.Flags = flags;
    return SUCCEEDED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
        static_cast<D3D12_RESOURCE_STATES>(initialState), nullptr, IID_PPV_ARGS(&output)));
}

bool CompileShaders() noexcept
{
    if (gVertexShader && gPixelShader && gSynthShader && gSynthBoundShader) return true;
    // Resolved at run time so no module gains a link dependency; the UI
    // renderer has normally loaded the system compiler already.
    HMODULE compiler = GetModuleHandleW(L"d3dcompiler_47.dll");
    if (!compiler) compiler = LoadLibraryExW(L"d3dcompiler_47.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    const auto compile = compiler ? reinterpret_cast<pD3DCompile>(GetProcAddress(compiler, "D3DCompile")) : nullptr;
    if (!compile) return false;
    ComPtr<ID3DBlob> errors;
    return SUCCEEDED(compile(kVertexShader, sizeof(kVertexShader) - 1, "hudless_vs", nullptr, nullptr,
            "main", "vs_5_0", 0, 0, &gVertexShader, &errors))
        && SUCCEEDED(compile(kPixelShader, sizeof(kPixelShader) - 1, "hudless_tint_ps", nullptr,
            nullptr, "main", "ps_5_0", 0, 0, &gPixelShader, &errors))
        && SUCCEEDED(compile(kPixelShader, sizeof(kPixelShader) - 1, "hudless_synth_ps", nullptr,
            nullptr, "synth", "ps_5_0", 0, 0, &gSynthShader, &errors))
        && SUCCEEDED(compile(kPixelShader, sizeof(kPixelShader) - 1, "hudless_synth_bound_ps", nullptr,
            nullptr, "synthBound", "ps_5_0", 0, 0, &gSynthBoundShader, &errors));
}

bool CreateRootSignature(ID3D12Device* device, ComPtr<ID3D12RootSignature>& output) noexcept
{
    using SerializeFn = HRESULT(WINAPI*)(const D3D12_ROOT_SIGNATURE_DESC*, D3D_ROOT_SIGNATURE_VERSION,
        ID3DBlob**, ID3DBlob**);
    const HMODULE d3d12 = GetModuleHandleW(L"d3d12.dll");
    const auto serialize = d3d12
        ? reinterpret_cast<SerializeFn>(GetProcAddress(d3d12, "D3D12SerializeRootSignature")) : nullptr;
    if (!serialize) return false;
    D3D12_DESCRIPTOR_RANGE range{};
    range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    range.NumDescriptors = kSrvPerDraw;
    range.OffsetInDescriptorsFromTableStart = 0;
    D3D12_ROOT_PARAMETER parameters[3]{};
    parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameters[0].DescriptorTable.NumDescriptorRanges = 1;
    parameters[0].DescriptorTable.pDescriptorRanges = &range;
    parameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    parameters[1].Constants.Num32BitValues = 8;
    parameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    // Learned-alpha state (synth only; the other shaders do not declare u1).
    parameters[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    parameters[2].Descriptor.ShaderRegister = 1;
    parameters[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_ROOT_SIGNATURE_DESC desc{};
    desc.NumParameters = 3;
    desc.pParameters = parameters;
    ComPtr<ID3DBlob> blob, errors;
    return SUCCEEDED(serialize(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &errors))
        && SUCCEEDED(device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(),
            IID_PPV_ARGS(&output)));
}

// Tint: blends magenta over the backbuffer. Synthesis: writes alpha unblended.
bool CreatePipelineState(ID3DBlob* pixelShader, DXGI_FORMAT rtvFormat, bool blend,
    ComPtr<ID3D12PipelineState>& output) noexcept
{
    D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};
    desc.pRootSignature = gPipe.root.Get();
    desc.VS = {gVertexShader->GetBufferPointer(), gVertexShader->GetBufferSize()};
    desc.PS = {pixelShader->GetBufferPointer(), pixelShader->GetBufferSize()};
    auto& target = desc.BlendState.RenderTarget[0];
    target.BlendEnable = blend ? TRUE : FALSE;
    target.SrcBlend = D3D12_BLEND_SRC_ALPHA;
    target.DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
    target.BlendOp = D3D12_BLEND_OP_ADD;
    target.SrcBlendAlpha = D3D12_BLEND_ZERO;
    target.DestBlendAlpha = D3D12_BLEND_ONE;
    target.BlendOpAlpha = D3D12_BLEND_OP_ADD;
    target.LogicOp = D3D12_LOGIC_OP_NOOP;
    target.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    desc.SampleMask = UINT_MAX;
    desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    desc.RasterizerState.DepthClipEnable = TRUE;
    desc.DepthStencilState.DepthEnable = FALSE;
    desc.DepthStencilState.StencilEnable = FALSE;
    desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    desc.NumRenderTargets = 1;
    desc.RTVFormats[0] = rtvFormat;
    desc.SampleDesc.Count = 1;
    ComPtr<ID3D12PipelineState> pso;
    if (FAILED(gPipe.device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&pso)))) return false;
    Retire(output);
    output = std::move(pso);
    return true;
}

// Device-level objects shared by both passes. Caller holds gMutex.
bool EnsureDevice(ID3D12Device* device) noexcept
{
    if (gPipe.device.Get() == device) return CompileShaders();
    // A previous device's work may still be executing.
    for (auto& allocator : gPipe.allocators) Retire(allocator);
    for (auto& held : gPipe.heldSources) Retire(held);
    for (auto& alpha : gPipe.alpha) Retire(alpha);
    Retire(gPipe.list);
    Retire(gPipe.fence);
    Retire(gPipe.srvHeap);
    Retire(gPipe.rtvHeap);
    Retire(gPipe.root);
    Retire(gPipe.pso);
    Retire(gPipe.synthPso);
    Retire(gPipe.synthBoundPso);
    Retire(gPipe.state);
    Retire(gPipe.scratch);
    Retire(gPipe.presentCopy);
    gPipe = Pipeline{};
    for (auto& allocator : gPipe.allocators)
        if (FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator))))
            return false;
    D3D12_DESCRIPTOR_HEAP_DESC srv{};
    srv.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    srv.NumDescriptors = kSrvPerDraw * kRing;
    srv.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    D3D12_DESCRIPTOR_HEAP_DESC rtv{};
    rtv.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    rtv.NumDescriptors = 1 + kAlphaRing;
    if (FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
            gPipe.allocators[0].Get(), nullptr, IID_PPV_ARGS(&gPipe.list)))
        || FAILED(gPipe.list->Close())
        || FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&gPipe.fence)))
        || FAILED(device->CreateDescriptorHeap(&srv, IID_PPV_ARGS(&gPipe.srvHeap)))
        || FAILED(device->CreateDescriptorHeap(&rtv, IID_PPV_ARGS(&gPipe.rtvHeap)))
        || !CreateRootSignature(device, gPipe.root))
        return false;
    gPipe.srvStep = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    gPipe.rtvStep = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    gPipe.device = device;
    return CompileShaders();
}

bool EnsureTintPipeline(ID3D12Device* device, DXGI_FORMAT rtvFormat) noexcept
{
    if (!EnsureDevice(device)) return false;
    if (gPipe.pso && gPipe.rtvFormat == rtvFormat) return true;
    if (!CreatePipelineState(gPixelShader.Get(), rtvFormat, true, gPipe.pso)) return false;
    gPipe.rtvFormat = rtvFormat;
    return true;
}

bool EnsureSynthPipeline(ID3D12Device* device) noexcept
{
    return EnsureDevice(device)
        && (gPipe.synthPso || CreatePipelineState(gSynthShader.Get(), DXGI_FORMAT_R8_UNORM, false, gPipe.synthPso))
        && (gPipe.synthBoundPso
            || CreatePipelineState(gSynthBoundShader.Get(), DXGI_FORMAT_R8_UNORM, false, gPipe.synthBoundPso));
}

// Final colour is copied before either pass reads it: a swapchain buffer may
// not allow shader reads, and the tint renders into it.
bool EnsureScratch(ID3D12Device* device, const D3D12_RESOURCE_DESC& desc) noexcept
{
    if (gPipe.scratch && gPipe.scratchFormat == desc.Format
        && gPipe.scratchWidth == desc.Width && gPipe.scratchHeight == desc.Height)
        return true;
    Retire(gPipe.scratch);
    if (!CreateTexture(device, desc.Format, static_cast<uint32_t>(desc.Width), desc.Height, gPipe.scratch))
        return false;
    gPipe.scratchFormat = desc.Format;
    gPipe.scratchWidth = static_cast<uint32_t>(desc.Width);
    gPipe.scratchHeight = desc.Height;
    return true;
}

bool EnsureAlpha(ID3D12Device* device, uint32_t width, uint32_t height) noexcept
{
    if (gPipe.alpha[0] && gPipe.alphaWidth == width && gPipe.alphaHeight == height) return true;
    for (auto& alpha : gPipe.alpha) Retire(alpha);
    gPipe.alphaLast = -1;
    gPipe.alphaNext = 0;
    for (auto& alpha : gPipe.alpha)
        if (!CreateTexture(device, DXGI_FORMAT_R8_UNORM, width, height, alpha,
                D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET, kShaderRead))
            return false;
    gPipe.alphaWidth = width;
    gPipe.alphaHeight = height;
    return true;
}

// Learned-alpha state for this size (zero-initialized; the first pass primes
// it). False means the per-frame alpha only; a failed size is not retried.
bool EnsureState(ID3D12Device* device, uint32_t width, uint32_t height) noexcept
{
    if (gPipe.stateWidth == width && gPipe.stateHeight == height) return gPipe.state != nullptr;
    Retire(gPipe.state);
    gPipe.stateWidth = width;
    gPipe.stateHeight = height;
    gPipe.statePrimed = false;
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = uint64_t(width) * height * kStateBytesPerPixel;
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    return SUCCEEDED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
        D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&gPipe.state)));
}

// A null resource writes a null descriptor (the shader never reads it).
void CreateSrv(ID3D12Resource* resource, DXGI_FORMAT format, D3D12_CPU_DESCRIPTOR_HANDLE handle) noexcept
{
    D3D12_SHADER_RESOURCE_VIEW_DESC view{};
    view.Format = resource ? TypedFormat(format) : DXGI_FORMAT_R8G8B8A8_UNORM;
    view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    view.Texture2D.MipLevels = 1;
    gPipe.device->CreateShaderResourceView(resource, &view, handle);
}

D3D12_CPU_DESCRIPTOR_HANDLE RtvHandle(uint32_t index) noexcept
{
    D3D12_CPU_DESCRIPTOR_HANDLE handle = gPipe.rtvHeap->GetCPUDescriptorHandleForHeapStart();
    handle.ptr += static_cast<SIZE_T>(index) * gPipe.rtvStep;
    return handle;
}

// States a compute command list cannot transition.
constexpr UINT kGraphicsOnlyStates = D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER
    | D3D12_RESOURCE_STATE_INDEX_BUFFER | D3D12_RESOURCE_STATE_RENDER_TARGET
    | D3D12_RESOURCE_STATE_DEPTH_WRITE | D3D12_RESOURCE_STATE_DEPTH_READ
    | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_STREAM_OUT
    | D3D12_RESOURCE_STATE_RESOLVE_DEST | D3D12_RESOURCE_STATE_RESOLVE_SOURCE;

void CopyRegion(ID3D12GraphicsCommandList* list, ID3D12Resource* destination, ID3D12Resource* source,
    UINT sourceState, const Latest& region) noexcept
{
    const bool transition = (sourceState & D3D12_RESOURCE_STATE_COPY_SOURCE) == 0;
    if (transition)
    {
        const auto barrier = Transition(source, sourceState, D3D12_RESOURCE_STATE_COPY_SOURCE);
        list->ResourceBarrier(1, &barrier);
    }
    D3D12_TEXTURE_COPY_LOCATION to{};
    to.pResource = destination;
    to.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    D3D12_TEXTURE_COPY_LOCATION from{};
    from.pResource = source;
    from.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    D3D12_BOX box{region.left, region.top, 0, region.left + region.width, region.top + region.height, 1};
    list->CopyTextureRegion(&to, 0, 0, 0, &from, &box);
    if (transition)
    {
        const auto barrier = Transition(source, D3D12_RESOURCE_STATE_COPY_SOURCE, sourceState);
        list->ResourceBarrier(1, &barrier);
    }
}

struct Selection
{
    std::array<ID3D12Resource*, kCandidates> hudless{};
    uint32_t count = 0;
    bool paired = false;
    bool deferred = false;
};

enum class Pick : uint32_t { eOk, eWaiting, eUnpaired, eUnsupported, eFailed };

// Chooses this Present's HUDless images. latestOnly (synthesis) wants exactly
// one image: the presented frame's copy, or the latest when tags carry no frame
// index. Frame-indexed copies must match exactly; a guess would tint or
// synthesize scene pixels whenever the camera moves. Caller holds gMutex.
Pick SelectHudless(ID3D12Device* device, bool latestOnly, Selection& out) noexcept
{
    out = {};
    if (!gLatest.sequence) return Pick::eWaiting;
    uint32_t present = 0;
    const bool markers = hudless_probe::CurrentPresentFrame(present);
    if (gLatest.copiedAtTag)
    {
        if (!hudless_probe::SameNativeDevice(gTagCopy.device.Get(), device)) return Pick::eUnsupported;
        bool framed = false;
        // Newest first; the ring index before next is the latest copy.
        for (uint32_t age = 0; age < kTagSlots; ++age)
        {
            const uint32_t index = (gTagCopy.next + kTagSlots - 1 - age) % kTagSlots;
            if (!gTagCopy.sequences[index] || !gTagCopy.textures[index]) continue;
            framed = framed || gTagCopy.frames[index] != hudless_probe::kNoFrame;
            if (markers && gTagCopy.frames[index] == present)
            {
                out = {};
                out.hudless[0] = gTagCopy.textures[index].Get();
                out.count = 1;
                out.paired = true;
                return Pick::eOk;
            }
            if (out.count < (latestOnly ? 1u : kCandidates))
                out.hudless[out.count++] = gTagCopy.textures[index].Get();
        }
        if (framed && (markers || latestOnly))
        {
            out = {};
            return Pick::eUnpaired;
        }
        return out.count ? Pick::eOk : Pick::eWaiting;
    }
    ComPtr<ID3D12Device> resourceDevice;
    if (!gLatest.resource || FAILED(gLatest.resource->GetDevice(IID_PPV_ARGS(&resourceDevice)))
        || !hudless_probe::SameNativeDevice(resourceDevice.Get(), device))
        return Pick::eUnsupported;
    const bool framed = gLatest.frame != hudless_probe::kNoFrame;
    if (framed && ((markers && gLatest.frame != present) || (!markers && latestOnly)))
        return Pick::eUnpaired;
    if (!gPipe.presentCopy || gPipe.presentCopyFormat != gLatest.format
        || gPipe.presentCopyWidth != gLatest.width || gPipe.presentCopyHeight != gLatest.height)
    {
        Retire(gPipe.presentCopy);
        if (!CreateTexture(device, gLatest.format, gLatest.width, gLatest.height, gPipe.presentCopy))
            return Pick::eFailed;
        gPipe.presentCopyFormat = gLatest.format;
        gPipe.presentCopyWidth = gLatest.width;
        gPipe.presentCopyHeight = gLatest.height;
    }
    out.hudless[0] = gPipe.presentCopy.Get();
    out.count = 1;
    out.paired = markers && framed;
    out.deferred = true;
    return Pick::eOk;
}

enum class Begin : uint32_t { eOk, eBusy, eFailed };

// Opens the shared command list on the next ring slot. Caller holds gMutex.
Begin BeginList(ID3D12PipelineState* pso, uint32_t& slot) noexcept
{
    slot = gPipe.slot;
    if (gPipe.fence->GetCompletedValue() < gPipe.fenceValues[slot]) return Begin::eBusy;
    gPipe.heldSources[slot].Reset();
    auto& allocator = gPipe.allocators[slot];
    if (FAILED(allocator->Reset()) || FAILED(gPipe.list->Reset(allocator.Get(), pso))) return Begin::eFailed;
    return Begin::eOk;
}

bool Submit(ID3D12CommandQueue* queue, uint32_t slot) noexcept
{
    if (FAILED(gPipe.list->Close())) return false;
    ID3D12CommandList* lists[] = {gPipe.list.Get()};
    queue->ExecuteCommandLists(1, lists);
    const UINT64 value = gPipe.nextFence++;
    gPipe.fenceValues[slot] = value;
    if (FAILED(queue->Signal(gPipe.fence.Get(), value)))
    {
        // The submission may still run; never reuse or release its objects.
        gBroken = true;
        return false;
    }
    gPipe.slot = (slot + 1) % kRing;
    return true;
}

// Records the Present-time HUDless copy (if any), copies final colour into the
// scratch texture and leaves the backbuffer in COPY_SOURCE, scratch and the
// HUDless images readable, and their descriptors written. Returns the table.
D3D12_GPU_DESCRIPTOR_HANDLE RecordInputs(ID3D12GraphicsCommandList* list, uint32_t slot,
    ID3D12Resource* backbuffer, DXGI_FORMAT finalFormat, const Selection& selection) noexcept
{
    if (selection.deferred)
    {
        CopyRegion(list, selection.hudless[0], gLatest.resource.Get(), gLatest.state, gLatest);
        gPipe.heldSources[slot] = gLatest.resource;
    }
    D3D12_RESOURCE_BARRIER barriers[1 + kCandidates] = {
        Transition(backbuffer, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_SOURCE,
            D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES),
    };
    list->ResourceBarrier(1, barriers);
    list->CopyResource(gPipe.scratch.Get(), backbuffer);
    barriers[0] = Transition(gPipe.scratch.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    for (uint32_t index = 0; index < selection.count; ++index)
        barriers[1 + index] = Transition(selection.hudless[index], D3D12_RESOURCE_STATE_COPY_DEST,
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    list->ResourceBarrier(1 + selection.count, barriers);

    D3D12_CPU_DESCRIPTOR_HANDLE cpu = gPipe.srvHeap->GetCPUDescriptorHandleForHeapStart();
    D3D12_GPU_DESCRIPTOR_HANDLE gpu = gPipe.srvHeap->GetGPUDescriptorHandleForHeapStart();
    cpu.ptr += static_cast<SIZE_T>(slot) * kSrvPerDraw * gPipe.srvStep;
    gpu.ptr += static_cast<UINT64>(slot) * kSrvPerDraw * gPipe.srvStep;
    CreateSrv(gPipe.scratch.Get(), finalFormat, cpu);
    for (uint32_t index = 0; index < kCandidates; ++index)
    {
        D3D12_CPU_DESCRIPTOR_HANDLE candidate = cpu;
        candidate.ptr += static_cast<SIZE_T>(1 + index) * gPipe.srvStep;
        CreateSrv(index < selection.count ? selection.hudless[index] : nullptr, gLatest.format, candidate);
    }
    return gpu;
}

// Returns scratch and the HUDless images to COPY_DEST after the draw.
void ReleaseInputs(ID3D12GraphicsCommandList* list, const Selection& selection) noexcept
{
    D3D12_RESOURCE_BARRIER barriers[1 + kCandidates] = {
        Transition(gPipe.scratch.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
            D3D12_RESOURCE_STATE_COPY_DEST),
    };
    for (uint32_t index = 0; index < selection.count; ++index)
        barriers[1 + index] = Transition(selection.hudless[index], D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
            D3D12_RESOURCE_STATE_COPY_DEST);
    list->ResourceBarrier(1 + selection.count, barriers);
}

// Learned-alpha state for a synth draw; the per-frame shader takes none.
struct SynthArgs
{
    D3D12_GPU_VIRTUAL_ADDRESS state = 0;
    bool prime = false; // Record this frame without learning from the old contents.
};

void DrawFullscreen(ID3D12GraphicsCommandList* list, D3D12_GPU_DESCRIPTOR_HANDLE table,
    D3D12_CPU_DESCRIPTOR_HANDLE rtv, uint32_t width, uint32_t height, uint32_t count,
    const SynthArgs* synth = nullptr) noexcept
{
    uint32_t constants[8]{};
    std::memcpy(&constants[0], &kThreshold, sizeof(float));
    std::memcpy(&constants[1], &kStrength, sizeof(float));
    constants[2] = hudless_probe::HudlessRelation();
    constants[3] = count;
    constants[4] = width;
    constants[5] = synth && synth->prime ? 1u : 0u;
    ID3D12DescriptorHeap* heaps[] = {gPipe.srvHeap.Get()};
    list->SetGraphicsRootSignature(gPipe.root.Get());
    list->SetDescriptorHeaps(1, heaps);
    list->SetGraphicsRootDescriptorTable(0, table);
    list->SetGraphicsRoot32BitConstants(1, 8, constants, 0);
    if (synth && synth->state) list->SetGraphicsRootUnorderedAccessView(2, synth->state);
    const D3D12_VIEWPORT viewport{0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height), 0.0f, 1.0f};
    const D3D12_RECT scissor{0, 0, static_cast<LONG>(width), static_cast<LONG>(height)};
    list->RSSetViewports(1, &viewport);
    list->RSSetScissorRects(1, &scissor);
    list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    list->DrawInstanced(3, 1, 0, 0);
}

// Clears alpha slot 0 to "no UI" so a UI buffer can be tagged before the first
// alpha is derived. Caller holds gMutex.
bool ClearAlpha(ID3D12CommandQueue* queue) noexcept
{
    uint32_t slot = 0;
    if (BeginList(nullptr, slot) != Begin::eOk) return false;
    auto* list = gPipe.list.Get();
    ID3D12Resource* alpha = gPipe.alpha[0].Get();
    auto barrier = Transition(alpha, kShaderRead, D3D12_RESOURCE_STATE_RENDER_TARGET);
    list->ResourceBarrier(1, &barrier);
    D3D12_RENDER_TARGET_VIEW_DESC rtvDesc{};
    rtvDesc.Format = DXGI_FORMAT_R8_UNORM;
    rtvDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
    const D3D12_CPU_DESCRIPTOR_HANDLE rtv = RtvHandle(1);
    gPipe.device->CreateRenderTargetView(alpha, &rtvDesc, rtv);
    const float none[4]{};
    list->ClearRenderTargetView(rtv, none, 0, nullptr);
    barrier = Transition(alpha, D3D12_RESOURCE_STATE_RENDER_TARGET, kShaderRead);
    list->ResourceBarrier(1, &barrier);
    if (!Submit(queue, slot)) return false;
    gPipe.alphaLast = 0;
    gPipe.alphaNext = 1;
    return true;
}

// Releases the game's HUDless reference once no pass needs it. Caller holds gMutex.
void ReleaseUnused() noexcept
{
    if (!gEnabled.load(std::memory_order_acquire) && !gSynthesisActive.load(std::memory_order_acquire))
        gLatest.resource.Reset();
}
}

void SetEnabled(bool enabled) noexcept
{
    (void)Enabled(); // Apply the environment default first so it cannot override this.
    gEnabled.store(enabled, std::memory_order_release);
    if (enabled) return;
    SetStatus(Status::eOff);
    std::lock_guard lock(gMutex);
    ReleaseUnused();
}

bool Enabled() noexcept
{
    // Diagnostic opt-in for unattended checks: RTXMFG_HUDLESS_TINT=1 starts
    // the session with the tint enabled.
    static const bool fromEnvironment = [] {
        wchar_t value[4]{};
        const bool on = GetEnvironmentVariableW(L"RTXMFG_HUDLESS_TINT", value, 4) == 1 && value[0] == L'1';
        if (on) gEnabled.store(true, std::memory_order_release);
        return on;
    }();
    (void)fromEnvironment;
    return gEnabled.load(std::memory_order_acquire);
}

bool WantsHudless() noexcept
{
    return Enabled() || gSynthesisActive.load(std::memory_order_acquire);
}

void ObserveHudless(void* resource, uint32_t state, uint32_t lifecycle,
    uint32_t extentLeft, uint32_t extentTop, uint32_t extentWidth, uint32_t extentHeight,
    void* commandList, uint32_t frame) noexcept
{
    if (!WantsHudless() || !resource) return;
    std::lock_guard lock(gMutex);
    ComPtr<ID3D12Resource> texture;
    if (FAILED(static_cast<IUnknown*>(resource)->QueryInterface(IID_PPV_ARGS(&texture))) || !texture)
        return SetStatus(Status::eUnsupported);
    const D3D12_RESOURCE_DESC desc = texture->GetDesc();
    if (desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || desc.SampleDesc.Count != 1
        || desc.Width > UINT32_MAX || state == UINT_MAX)
        return SetStatus(Status::eUnsupported);
    Latest next{};
    next.format = desc.Format;
    next.left = extentLeft;
    next.top = extentTop;
    next.width = extentWidth ? extentWidth : static_cast<uint32_t>(desc.Width);
    next.height = extentHeight ? extentHeight : desc.Height;
    next.frame = frame;
    if (next.left > desc.Width || next.width > desc.Width - next.left
        || next.top > desc.Height || next.height > desc.Height - next.top)
        return SetStatus(Status::eShapeMismatch);

    if (lifecycle == kOnlyValidNow)
    {
        // Streamline copies these on the tagging list; so must we.
        ComPtr<ID3D12GraphicsCommandList> list;
        ComPtr<ID3D12Device> listDevice, resourceDevice;
        if (!commandList || FAILED(static_cast<IUnknown*>(commandList)->QueryInterface(IID_PPV_ARGS(&list)))
            || FAILED(list->GetDevice(IID_PPV_ARGS(&listDevice)))
            || FAILED(texture->GetDevice(IID_PPV_ARGS(&resourceDevice)))
            || !hudless_probe::SameNativeDevice(listDevice.Get(), resourceDevice.Get()))
            return SetStatus(Status::eUnsupported);
        const auto type = list->GetType();
        const bool transition = (state & D3D12_RESOURCE_STATE_COPY_SOURCE) == 0;
        const bool recordable = type == D3D12_COMMAND_LIST_TYPE_DIRECT
            || (type == D3D12_COMMAND_LIST_TYPE_COMPUTE && (!transition || !(state & kGraphicsOnlyStates)));
        if (!recordable) return SetStatus(Status::eUnsupported);
        if (gTagCopy.device.Get() != listDevice.Get() || gTagCopy.format != desc.Format
            || gTagCopy.width != next.width || gTagCopy.height != next.height)
        {
            for (auto& old : gTagCopy.textures) Retire(old);
            gTagCopy = TagCopy{};
            for (auto& created : gTagCopy.textures)
                if (!CreateTexture(listDevice.Get(), desc.Format, next.width, next.height, created))
                {
                    for (auto& partial : gTagCopy.textures) Retire(partial);
                    gTagCopy = TagCopy{};
                    return SetStatus(Status::eFailed);
                }
            gTagCopy.device = listDevice;
            gTagCopy.format = desc.Format;
            gTagCopy.width = next.width;
            gTagCopy.height = next.height;
        }
        const uint32_t target = gTagCopy.next;
        CopyRegion(list.Get(), gTagCopy.textures[target].Get(), texture.Get(), state, next);
        gTagCopy.sequences[target] = gLatest.sequence + 1;
        gTagCopy.frames[target] = frame;
        gTagCopy.next = (target + 1) % kTagSlots;
        next.copiedAtTag = true;
    }
    else
    {
        next.resource = texture;
        next.state = state;
    }
    next.sequence = gLatest.sequence + 1;
    gLatest = std::move(next);
}

void Draw(ID3D12Device* device, ID3D12CommandQueue* queue, ID3D12Resource* backbuffer) noexcept
{
    if (!Enabled() || !device || !queue || !backbuffer) return;
    std::lock_guard lock(gMutex);
    if (gBroken) return SetStatus(Status::eFailed);
    if (!gLatest.sequence) return SetStatus(Status::eWaiting);
    // A Present-time HUDless is read once, at the Present it was tagged for.
    if (!gLatest.copiedAtTag && gLatest.sequence == gDrawnSequence) return;
    const D3D12_RESOURCE_DESC desc = backbuffer->GetDesc();
    if (desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || desc.Width != gLatest.width
        || desc.Height != gLatest.height)
        return SetStatus(Status::eShapeMismatch);
    const DXGI_FORMAT rtvFormat = TypedFormat(desc.Format);
    if (!EnsureTintPipeline(device, rtvFormat) || !EnsureScratch(device, desc))
        return SetStatus(Status::eFailed);

    Selection selection;
    switch (SelectHudless(device, false, selection))
    {
    case Pick::eOk: break;
    case Pick::eUnpaired: return; // This Present's own copy is missing.
    case Pick::eWaiting: return SetStatus(Status::eWaiting);
    case Pick::eUnsupported: return SetStatus(Status::eUnsupported);
    default: return SetStatus(Status::eFailed);
    }
    uint32_t slot = 0;
    const Begin begin = BeginList(gPipe.pso.Get(), slot);
    if (begin == Begin::eBusy) return;
    if (begin != Begin::eOk) return SetStatus(Status::eFailed);
    auto* list = gPipe.list.Get();
    const auto table = RecordInputs(list, slot, backbuffer, desc.Format, selection);
    auto barrier = Transition(backbuffer, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET,
        D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES);
    list->ResourceBarrier(1, &barrier);
    D3D12_RENDER_TARGET_VIEW_DESC rtvDesc{};
    rtvDesc.Format = rtvFormat;
    rtvDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
    const D3D12_CPU_DESCRIPTOR_HANDLE rtv = RtvHandle(0);
    device->CreateRenderTargetView(backbuffer, &rtvDesc, rtv);
    DrawFullscreen(list, table, rtv, static_cast<uint32_t>(desc.Width), desc.Height, selection.count);
    barrier = Transition(backbuffer, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT,
        D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES);
    list->ResourceBarrier(1, &barrier);
    ReleaseInputs(list, selection);
    if (!Submit(queue, slot)) return SetStatus(Status::eFailed);
    gDrawnSequence = gLatest.sequence;
    gFramesDrawn.fetch_add(1, std::memory_order_relaxed);
    if (selection.paired) gPairedFrames.fetch_add(1, std::memory_order_relaxed);
    SetStatus(Status::eDrawing);
}

const char* StateText() noexcept
{
    switch (static_cast<Status>(gStatus.load(std::memory_order_acquire)))
    {
    case Status::eWaiting: return "waiting for HUDless";
    case Status::eDrawing: return "drawing";
    case Status::eShapeMismatch: return "HUDless size differs from final colour";
    case Status::eUnsupported: return "HUDless not usable here";
    case Status::eFailed: return "renderer setup failed";
    default: return "off";
    }
}

uint64_t FramesDrawn() noexcept
{
    return gFramesDrawn.load(std::memory_order_relaxed);
}

uint64_t PairedFrames() noexcept
{
    return gPairedFrames.load(std::memory_order_relaxed);
}

void SetUiTagger(UiTagger tagger) noexcept
{
    gTagger.store(tagger, std::memory_order_release);
}

bool SynthesisSupported() noexcept
{
    return gTagger.load(std::memory_order_acquire) != nullptr;
}

void SetSynthesisActive(bool active) noexcept
{
    if (gSynthesisActive.exchange(active, std::memory_order_acq_rel) == active) return;
    if (active)
    {
        // Callers may hold the probe's lock, so the pass applies this itself.
        gRelearn.store(true, std::memory_order_release);
        return;
    }
    SetSynthStatus(SynthStatus::eOff);
    std::lock_guard lock(gMutex);
    ReleaseUnused();
}

bool SynthesisActive() noexcept
{
    return gSynthesisActive.load(std::memory_order_acquire);
}

void SynthesizeUi(ID3D12Device* device, ID3D12CommandQueue* queue, ID3D12Resource* backbuffer,
    bool reuseOnly) noexcept
{
    if (!SynthesisActive() || !device || !queue || !backbuffer) return;
    const UiTagger tagger = gTagger.load(std::memory_order_acquire);
    if (!tagger) return SetSynthStatus(SynthStatus::eNoTagger);
    std::lock_guard lock(gMutex);
    uint32_t present = hudless_probe::kNoFrame;
    if (!hudless_probe::CurrentPresentFrame(present)) present = hudless_probe::kNoFrame;
    // Tags the last derived alpha again for this Present.
    const auto tagLast = [&](SynthStatus why) noexcept {
        if (gPipe.alphaLast < 0) return SetSynthStatus(why);
        ID3D12Resource* alpha = gPipe.alpha[static_cast<uint32_t>(gPipe.alphaLast)].Get();
        if (!alpha || !tagger(alpha, kShaderRead, gPipe.alphaWidth, gPipe.alphaHeight,
                DXGI_FORMAT_R8_UNORM, present))
            return SetSynthStatus(SynthStatus::eRejected);
        gSynthReused.fetch_add(1, std::memory_order_relaxed);
        SetSynthStatus(why);
    };
    if (gBroken) return tagLast(SynthStatus::eFailed);
    if (reuseOnly && gPipe.alphaLast >= 0) return tagLast(SynthStatus::eReused);
    const D3D12_RESOURCE_DESC desc = backbuffer->GetDesc();
    if (desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || desc.Width > UINT32_MAX)
        return tagLast(SynthStatus::eUnsupported);
    const uint32_t width = static_cast<uint32_t>(desc.Width);
    const uint32_t height = desc.Height;
    // The UI buffer exists from the first Present: until an alpha is derived,
    // a cleared one (no UI) is tagged.
    if (!EnsureSynthPipeline(device) || !EnsureAlpha(device, width, height))
        return tagLast(SynthStatus::eFailed);
    if (gPipe.alphaLast < 0 && !ClearAlpha(queue)) return SetSynthStatus(SynthStatus::eFailed);
    if (reuseOnly) return tagLast(SynthStatus::eReused);
    if (!gLatest.sequence) return tagLast(SynthStatus::eWaiting);
    if (width != gLatest.width || height != gLatest.height) return tagLast(SynthStatus::eShapeMismatch);
    // A Present-time HUDless belongs to one Present; without a new tag this
    // Present has no HUDless of its own.
    if (!gLatest.copiedAtTag && gLatest.sequence == gSynthSequence) return tagLast(SynthStatus::eReused);
    if (!EnsureScratch(device, desc)) return tagLast(SynthStatus::eFailed);

    Selection selection;
    switch (SelectHudless(device, true, selection))
    {
    case Pick::eOk: break;
    case Pick::eUnsupported: return tagLast(SynthStatus::eUnsupported);
    case Pick::eFailed: return tagLast(SynthStatus::eFailed);
    default: return tagLast(SynthStatus::eWaiting);
    }
    if (gRelearn.exchange(false, std::memory_order_acq_rel)) gPipe.statePrimed = false;
    const bool learn = EnsureState(device, width, height);
    uint32_t slot = 0;
    const Begin begin = BeginList(learn ? gPipe.synthPso.Get() : gPipe.synthBoundPso.Get(), slot);
    if (begin != Begin::eOk)
        return tagLast(begin == Begin::eBusy ? SynthStatus::eReused : SynthStatus::eFailed);
    auto* list = gPipe.list.Get();
    const uint32_t target = gPipe.alphaNext;
    ID3D12Resource* alpha = gPipe.alpha[target].Get();
    const auto table = RecordInputs(list, slot, backbuffer, desc.Format, selection);
    D3D12_RESOURCE_BARRIER barriers[3] = {
        Transition(backbuffer, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_PRESENT,
            D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES),
        Transition(alpha, kShaderRead, D3D12_RESOURCE_STATE_RENDER_TARGET),
        Transition(gPipe.state.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
    };
    list->ResourceBarrier(learn ? 3 : 2, barriers);
    D3D12_RENDER_TARGET_VIEW_DESC rtvDesc{};
    rtvDesc.Format = DXGI_FORMAT_R8_UNORM;
    rtvDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
    const D3D12_CPU_DESCRIPTOR_HANDLE rtv = RtvHandle(1 + target);
    device->CreateRenderTargetView(alpha, &rtvDesc, rtv);
    SynthArgs args{};
    if (learn)
    {
        args.state = gPipe.state->GetGPUVirtualAddress();
        args.prime = !gPipe.statePrimed;
    }
    DrawFullscreen(list, table, rtv, width, height, 1, &args);
    barriers[0] = Transition(alpha, D3D12_RESOURCE_STATE_RENDER_TARGET, kShaderRead);
    barriers[1] = Transition(gPipe.state.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);
    list->ResourceBarrier(learn ? 2 : 1, barriers);
    ReleaseInputs(list, selection);
    if (!Submit(queue, slot)) return SetSynthStatus(SynthStatus::eFailed);
    gSynthSequence = gLatest.sequence;
    if (args.prime) gSynthPrimes.fetch_add(1, std::memory_order_relaxed);
    gPipe.statePrimed = learn;
    gSynthAlphaMode.store(static_cast<uint32_t>(learn ? AlphaMode::eLearned : AlphaMode::ePerFrame),
        std::memory_order_release);
    if (!tagger(alpha, kShaderRead, width, height, DXGI_FORMAT_R8_UNORM, present))
        return SetSynthStatus(SynthStatus::eRejected); // The slot is reused next time.
    gPipe.alphaLast = static_cast<int32_t>(target);
    gPipe.alphaNext = (target + 1) % kAlphaRing;
    gSynthFrames.fetch_add(1, std::memory_order_relaxed);
    gLastFreshTagTick.store(GetTickCount64(), std::memory_order_release);
    SetSynthStatus(SynthStatus::eTagging);
}

bool SynthesisHealthy() noexcept
{
    const uint64_t tick = gLastFreshTagTick.load(std::memory_order_acquire);
    return SynthesisActive() && tick && GetTickCount64() - tick <= kHealthyMs;
}

const char* SynthesisStateText() noexcept
{
    switch (static_cast<SynthStatus>(gSynthStatus.load(std::memory_order_acquire)))
    {
    case SynthStatus::eWaiting: return "waiting for this frame's HUDless";
    case SynthStatus::eTagging: return "tagging";
    case SynthStatus::eReused: return "reusing the last UI";
    case SynthStatus::eShapeMismatch: return "HUDless size differs from final colour";
    case SynthStatus::eUnsupported: return "HUDless not usable here";
    case SynthStatus::eFailed: return "renderer setup failed";
    case SynthStatus::eRejected: return "Streamline rejected the UI tag";
    case SynthStatus::eNoTagger: return "Streamline tag hook unavailable";
    default: return "off";
    }
}

uint64_t SynthesizedFrames() noexcept
{
    return gSynthFrames.load(std::memory_order_relaxed);
}

uint64_t SynthesisReusedFrames() noexcept
{
    return gSynthReused.load(std::memory_order_relaxed);
}

const char* SynthesisAlphaText() noexcept
{
    switch (static_cast<AlphaMode>(gSynthAlphaMode.load(std::memory_order_acquire)))
    {
    case AlphaMode::eLearned: return "learned";
    case AlphaMode::ePerFrame: return "per-frame";
    default: return "none";
    }
}

uint64_t SynthesisPrimes() noexcept
{
    return gSynthPrimes.load(std::memory_order_relaxed);
}
}
