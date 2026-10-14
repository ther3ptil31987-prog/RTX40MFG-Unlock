#pragma once
#include <Windows.h>
#include <cstdint>
#include <string>
#include <xefg_swapchain_d3d12.h>
#include "sdk_pacing.h"
#include "xess_mod.h"

struct IDXGISwapChain1;

namespace xess_capture
{
// dev.37 diagnostic: what the XeFG SDK actually presents. Opt-in only: environment
// RTXMFG_XESS_CAPTURE=1, or a file RTXMFG-capture.enable beside the mod DLL.
// Ctrl+Shift+F9, after a short countdown, records:
//  - a centre crop and a top-left corner crop of every output of kBatches generating
//    batches, copied from the back buffer on the SDK's presentation queue right before
//    its Present1 (queued ahead of the flip meter's gate Wait, so the flip is not delayed);
//  - the game's motion-vector input of kMotionFrames frames and its depth of kDepthFrames
//    (copied in the game's own command list at its XeFG resource tag);
//  - the frame constants the game passes for each captured frame (after any matrix fix).
// beta.8: one pass with the startup input fixes (xess_mod.h), into
// %TEMP%\RTXMFG-capture-<time>\capture\ (index.json, frames.bin, corners.bin, mv<N>.bin, depth<N>.bin,
// hudless<N>.bin). Under OptiScaler its own XeFG outputs are recorded through the observed plain owner;
// their input tags go straight to the provider and are not recorded.
// Analyzer: build\v1.5-flipmeter-20261006\analyze_present_capture.py.
bool Enabled() noexcept;
// dev.46: a key=value line of RTXMFG-capture.enable that applies at startup (xefg_model=port|2x,
// optiscaler_model=port|2x);
// empty when the file or the key is absent. Lines starting with # are comments.
std::string StartupOption(const char* key) noexcept;
// Registers the SDK hooks and starts the hotkey and writer threads (once; nothing
// happens when capture is not enabled).
void Start() noexcept;

// xess_mod hooks (game threads), after the call reached the SDK.
void OnFrameConstants(uint32_t presentId, const xefg_swapchain_frame_constant_data_t& data) noexcept;
void OnTagResource(ID3D12CommandList* list, uint32_t presentId, const xefg_swapchain_d3d12_resource_data_t& data) noexcept;

// sdk_pacing::PresentCaptureHooks (SDK presenting threads).
void BeforeOutput(sdk_pacing::OutputKind kind, uint32_t presentId, ID3D12CommandQueue* queue) noexcept;
void BeforePresent1(IDXGISwapChain1* swapchain) noexcept;

#if defined(RTXMFG_CAPTURE_TESTING)
// Enables capture with output under `root` and starts the writer (no hotkey thread).
void TestEnable(const wchar_t* root) noexcept;
void TestRequest() noexcept; // as the hotkey
int TestState() noexcept;    // 0 idle, 1 requested, 2 preparing, 3 countdown, 4 capturing, 5 writing
bool TestWaitState(int state, DWORD milliseconds) noexcept;
std::wstring TestLastFolder() noexcept; // the last capture's folder
std::string TestStartupOption(const wchar_t* path, const char* key) noexcept;
#endif
}
