// Included inside xess_mod; ownership, retirement and poisoning use core state.
#define FG_ROUTE(name, args) \
    auto& s = Get(); std::lock_guard lock(s.fgMutex); \
    auto* c = FindFg(s, handle); \
    if (!c) return s.retiredFg.contains(handle) ? kInvalid : (original.name ? original.name args : XEFG_SWAPCHAIN_RESULT_ERROR_UNSUPPORTED); \
    if (c->closing) return kInvalid; \
    if (c->poisoned) return XEFG_SWAPCHAIN_RESULT_ERROR_WRONG_CALL_ORDER; \
    auto& api = s.api; (void)api

xefg_swapchain_result_t Hook_xefgSwapChainGetProperties(xefg_swapchain_handle_t handle, xefg_swapchain_properties_t* output)
{ FG_ROUTE(xefgSwapChainGetProperties, (handle, output)); return api.xefgSwapChainGetProperties(handle, output); }
xefg_swapchain_result_t Hook_xefgSwapChainD3D12GetInitializationParameters(xefg_swapchain_handle_t handle,
    xefg_swapchain_d3d12_init_params_t* output)
{ FG_ROUTE(xefgSwapChainD3D12GetInitializationParameters, (handle, output)); return api.xefgSwapChainD3D12GetInitializationParameters(handle, output); }
xefg_swapchain_result_t Hook_xefgSwapChainD3D12GetProperties(xefg_swapchain_handle_t handle,
    const xefg_swapchain_d3d12_init_params_t* input, uint32_t width, uint32_t height, DXGI_FORMAT format,
    xefg_swapchain_properties_t* output)
{
    FG_ROUTE(xefgSwapChainD3D12GetProperties, (handle, input, width, height, format, output));
    xefg_swapchain_d3d12_init_params_t params{};
    if (input) { params = *input; params.maxInterpolatedFrames = kGeneratedCapacity; params.pPipelineLibrary = nullptr; }
    const auto result = api.xefgSwapChainD3D12GetProperties(handle, input ? &params : nullptr, width, height, format, output);
    if (!input) {
        c->resizePropertiesKnown = result == 0 && output && c->initialized;
        if (c->resizePropertiesKnown) c->resizeProperties = *output;
    }
    return result;
}
xefg_swapchain_result_t Hook_xefgSwapChainD3D12BuildPipelines(xefg_swapchain_handle_t handle,
    ID3D12PipelineLibrary* library, uint8_t blocking, uint32_t flags)
{
    FG_ROUTE(xefgSwapChainD3D12BuildPipelines, (handle, library, blocking, flags));
    // The private context builds and owns its own pipelines (an application cache may predate the model).
    return api.xefgSwapChainD3D12BuildPipelines(handle, nullptr, blocking, flags);
}
xefg_swapchain_result_t Hook_xefgSwapChainGetPipelineBuildStatus(xefg_swapchain_handle_t handle)
{ FG_ROUTE(xefgSwapChainGetPipelineBuildStatus, (handle)); return api.xefgSwapChainGetPipelineBuildStatus(handle); }
xefg_swapchain_result_t Hook_xefgSwapChainSetSceneChangeThreshold(xefg_swapchain_handle_t handle, float threshold)
{ FG_ROUTE(xefgSwapChainSetSceneChangeThreshold, (handle, threshold)); return api.xefgSwapChainSetSceneChangeThreshold(handle, threshold); }
xefg_swapchain_result_t Hook_xefgSwapChainEnableDebugFeature(xefg_swapchain_handle_t handle,
    xefg_swapchain_debug_feature_t feature, uint32_t enabled, void* argument)
{
    FG_ROUTE(xefgSwapChainEnableDebugFeature, (handle, feature, enabled, argument));
    return api.xefgSwapChainEnableDebugFeature(handle, feature, enabled, argument);
}
xefg_swapchain_result_t Hook_xefgSwapChainSetNumInterpolatedFrames(xefg_swapchain_handle_t handle, uint32_t count)
{
    FG_ROUTE(xefgSwapChainSetNumInterpolatedFrames, (handle, count));
    if (!c->initialized || c->capacity != kGeneratedCapacity) return XEFG_SWAPCHAIN_RESULT_ERROR_WRONG_CALL_ORDER;
    if (!count || count > c->capacity) return kArgument;
    if (c->gameGenerated != count) { c->gameGenerated = count; c->countPending = true; }
    ApplyPending(s, *c); Publish(s, c);
    return c->poisoned ? XEFG_SWAPCHAIN_RESULT_ERROR_UNSUPPORTED : XEFG_SWAPCHAIN_RESULT_SUCCESS;
}
xefg_swapchain_result_t Hook_xefgSwapChainSetUiCompositionState(xefg_swapchain_handle_t handle,
    xefg_swapchain_ui_composition_state_t value)
{
    FG_ROUTE(xefgSwapChainSetUiCompositionState, (handle, value));
    if (value != XEFG_SWAPCHAIN_UI_COMPOSITION_STATE_DISABLED && value != XEFG_SWAPCHAIN_UI_COMPOSITION_STATE_ENABLED) return kArgument;
    if (!c->initialized) return XEFG_SWAPCHAIN_RESULT_ERROR_WRONG_CALL_ORDER;
    const bool prior = c->gameComposition;
    const bool managed = c->compositionManagedByGame;
    c->compositionManagedByGame = true;
    c->gameComposition = value == XEFG_SWAPCHAIN_UI_COMPOSITION_STATE_ENABLED;
    const auto result = UpdateComposition(s, *c, c->tags.frame);
    if (result != 0) { c->gameComposition = prior; c->compositionManagedByGame = managed; }
    Publish(s, c);
    return result;
}
xefg_swapchain_result_t Hook_xefgSwapChainD3D12SetDescriptorHeap(xefg_swapchain_handle_t handle,
    ID3D12DescriptorHeap* heap, uint32_t offset)
{
    FG_ROUTE(xefgSwapChainD3D12SetDescriptorHeap, (handle, heap, offset));
    if (!c->initialized) return XEFG_SWAPCHAIN_RESULT_ERROR_WRONG_CALL_ORDER;
    if (!heap) return kArgument;
    ComPtr<ID3D12Device> owner;
    if (heap->GetDevice(IID_PPV_ARGS(&owner)) != S_OK || !SameDevice(c->device.Get(), owner.Get())) return kArgument;
    const auto desc = heap->GetDesc();
    const uint32_t step = c->device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    xefg_swapchain_properties_t properties{};
    const auto result = api.xefgSwapChainGetProperties(handle, &properties);
    if (result != 0) return result;
    if (desc.Type != D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV || !(desc.Flags & D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE)
        || !step || offset % step || uint64_t(offset)/step+properties.requiredDescriptorCount > desc.NumDescriptors) return kArgument;
    return api.xefgSwapChainD3D12SetDescriptorHeap(handle, heap, offset);
}
xefg_swapchain_result_t Hook_xefgSwapChainD3D12UpdateExternalHeapOnResize(xefg_swapchain_handle_t handle,
    ID3D12Heap* buffer, uint64_t bo, ID3D12Heap* texture, uint64_t to)
{
    FG_ROUTE(xefgSwapChainD3D12UpdateExternalHeapOnResize, (handle, buffer, bo, texture, to));
    if (!c->initialized || !c->resizePropertiesKnown) return XEFG_SWAPCHAIN_RESULT_ERROR_WRONG_CALL_ORDER;
    if (!HeapPair(*c, buffer, bo, texture, to, c->resizeProperties)) return kArgument;
    const auto result = api.xefgSwapChainD3D12UpdateExternalHeapOnResize(handle, buffer, bo, texture, to);
    if (result == 0) c->resizePropertiesKnown = false;
    return result;
}
#undef FG_ROUTE
xell_result_t Hook_xellGetSleepMode(xell_context_handle_t handle, xell_sleep_params_t* params)
{
    auto& state = Get(); LlLease lease(state, handle);
    if (lease.retired) return XELL_RESULT_ERROR_INVALID_CONTEXT;
    const auto function = LatencyApi(state, lease).xellGetSleepMode;
    return function ? function(handle, params) : XELL_RESULT_ERROR_UNSUPPORTED;
}
xell_result_t Hook_xellGetFramesReports(xell_context_handle_t handle, xell_frame_report_t* output)
{
    auto& state = Get(); LlLease lease(state, handle);
    if (lease.retired) return XELL_RESULT_ERROR_INVALID_CONTEXT;
    const auto function = LatencyApi(state, lease).xellGetFramesReports;
    return function ? function(handle, output) : XELL_RESULT_ERROR_UNSUPPORTED;
}
