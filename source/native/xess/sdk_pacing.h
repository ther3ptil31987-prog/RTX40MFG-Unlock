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
// (sdk_pacing_dlssg.h). The native swap chain's buffer count is unchanged. flipmeter.1:
// the SDK's presentation queue is also resolved for flip metering; without it the
// owner keeps plain pacing (log flip_metering_queue).
bool Register(void* handle, uint64_t generation, ID3D12Device* device, std::string& failure) noexcept;
// Clears the owner's DLSS-G batch-interval history on an accepted enable/count/resize transition.
void Reset(void* handle, uint64_t generation) noexcept;
// The SDK has joined its presenter threads before a successful Destroy returns.
void Unregister(void* handle, uint64_t generation) noexcept;
// flipmeter.1 (test build): NVIDIA driver flip metering for the plain owner, as sl.dlss_g uses it
// (presentFrame -> setFlipConfig, 2.14.1 0x5F040). Right before the first Present of each batch the
// SDK's presentation queue gets NvAPI_D3D12_SetFlipConfig with nFramesPerBatch = presented = the
// batch's outputs and timePerBatchMs = the DLSS-G batch-interval estimate (sdk_pacing_dlssg.h); any
// other Present on that presenter (frame generation off) turns it off again. While the driver accepts
// it with independent flip, each deadline after the batch's first output follows the configured
// spacing and is released innerLeadNs early, so the display hardware, not the presenting thread's
// wake-up, places the flip. setFlipConfig: the NVIDIA driver's NvAPI_D3D12_SetFlipConfig, resolved by
// the caller on an NVIDIA adapter (never a fakenvapi); nullptr keeps the path off. enabled: the user's
// switch, which Page Up toggles live (logged as flip_metering_toggle) after SetFlipToggleHotkey(true).
using SetFlipConfigFunction = int (__cdecl*)(IUnknown* queue, UINT vidPnSourceId, void* config);
void SetFlipMetering(SetFlipConfigFunction setFlipConfig, bool enabled, int64_t innerLeadNs) noexcept;
// V1.5.1: Page Up toggles the switch live only after the user's opt-in (OptiScaler.ini [XeFG]
// XeFGFlipMeteringHotkey = on); off by default, as Page Up is also OptiScaler's FPS overlay key.
void SetFlipToggleHotkey(bool enabled) noexcept;
}
