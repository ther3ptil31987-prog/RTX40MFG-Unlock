#pragma once
#include <xefg_swapchain_d3d12.h>
#include <xefg_swapchain_debug.h>
#include <xell_d3d12.h>

#define XESS_FG_IMPORTS(X) \
    X(xefgSwapChainDestroy) \
    X(xefgSwapChainD3D12CreateContext) \
    X(xefgSwapChainTagFrameConstants) \
    X(xefgSwapChainSetPresentId) \
    X(xefgSwapChainGetLastPresentStatus) \
    X(xefgSwapChainSetEnabled) \
    X(xefgSwapChainD3D12TagFrameResource) \
    X(xefgSwapChainD3D12GetSwapChainPtr) \
    X(xefgSwapChainD3D12InitFromSwapChain) \
    X(xefgSwapChainSetLatencyReduction) \
    X(xefgSwapChainSetLoggingCallback) \
    X(xefgSwapChainGetProperties) \
    X(xefgSwapChainD3D12GetProperties) \
    X(xefgSwapChainD3D12GetInitializationParameters) \
    X(xefgSwapChainSetNumInterpolatedFrames) \
    X(xefgSwapChainSetUiCompositionState) \
    X(xefgSwapChainD3D12BuildPipelines) \
    X(xefgSwapChainD3D12InitFromSwapChainDesc) \
    X(xefgSwapChainD3D12SetDescriptorHeap) \
    X(xefgSwapChainD3D12UpdateExternalHeapOnResize) \
    X(xefgSwapChainSetSceneChangeThreshold) \
    X(xefgSwapChainGetPipelineBuildStatus) \
    X(xefgSwapChainEnableDebugFeature)

#define XESS_LL_IMPORTS(X) \
    X(xellDestroyContext) \
    X(xellSetLoggingCallback) \
    X(xellAddMarkerData) \
    X(xellSleep) \
    X(xellSetSleepMode) \
    X(xellD3D12CreateContext) \
    X(xellGetSleepMode) \
    X(xellGetFramesReports)

namespace xess_mod
{
struct Api
{
#define DECLARE_API(name) decltype(&name) name{};
    XESS_FG_IMPORTS(DECLARE_API)
    XESS_LL_IMPORTS(DECLARE_API)
#undef DECLARE_API
};
extern Api original;
#define DECLARE_HOOK(name) decltype(name) Hook_##name;
XESS_FG_IMPORTS(DECLARE_HOOK)
XESS_LL_IMPORTS(DECLARE_HOOK)
#undef DECLARE_HOOK
}
