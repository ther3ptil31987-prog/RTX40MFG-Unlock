#pragma once
#include <windows.h>
#include <d3d12.h>
#include <cstdint>
#include <string>

struct IDXGISwapChain1;

namespace sdk_pacing
{
// Hooks the exact profiled provider's native Present, Pace and Deadline. Install before
// registering an owner. The published route and the two modules remain pinned for
// process lifetime; no hook is removed during play.
bool Install(HMODULE provider, std::string& failure) noexcept;
// Called under the backend's context lock after a successful SDK initialization;
// presentation callbacks never take that context lock. Plain owner: extra outputs go
// through the SDK's own pacing wait; deadlines follow the DLSS-G batch interval
// (sdk_pacing_dlssg.h). observe (diagnostic capture): the presentation queue and the
// capture-only Present1 slot are resolved, so the final-present capture sees this owner's
// outputs. Pacing and the native swap chain's buffer count are unchanged either way.
bool Register(void* handle, uint64_t generation, ID3D12Device* device, std::string& failure, bool observe = false) noexcept;
// dev.37 diagnostic final-present capture (xess/present_capture.h). beforeOutput runs for
// every native Present of an observed owner, on the presenting thread, before the SDK's
// native call: eFirst/eExtra/eReal are the outputs of a generating batch (first opens it,
// real closes it); eOther is any other Present on that presenter (frame generation off).
// queue is the SDK's presentation queue; presentId the high half of the SDK frame value,
// which is the XeLL PRESENT_START frame ID XeFG read for the batch, not the game's
// SetPresentId value (dev.35 Witcher log: 2146 vs 2). beforePresent1 runs right before the
// owner's native swap chain Present1 (not a test Present) reaches DXGI's own Present1 or,
// on a layered swap chain (dev.49, OptiScaler), the layer's, with that swap chain.
enum class OutputKind : uint32_t { eFirst = 0, eExtra = 1, eReal = 2, eOther = 3 };
struct PresentCaptureHooks
{
    void (*beforeOutput)(OutputKind kind, uint32_t presentId, ID3D12CommandQueue* queue) noexcept;
    void (*beforePresent1)(IDXGISwapChain1* swapchain) noexcept;
};
// The table must stay valid for the process lifetime; nullptr stops the calls.
void SetPresentCapture(const PresentCaptureHooks* hooks) noexcept;
// Clears the owner's DLSS-G batch-interval history on an accepted enable/count/resize transition.
void Reset(void* handle, uint64_t generation) noexcept;
// The SDK has joined its presenter threads before a successful Destroy returns.
void Unregister(void* handle, uint64_t generation) noexcept;
}
