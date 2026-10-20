#pragma once
#include "midpoint_fix.h"

namespace ampere_gpu
{
using LogCallback = midpoint_fix::LogCallback;
void SetLogCallback(LogCallback) noexcept;
bool ObserveD3D12Device(void*) noexcept;
bool ObserveAdapter(void*) noexcept;
// The VkPhysicalDevice LUID takes the D3D12 device's place in the same CUDA
// adapter verification. ObserveVulkanAdapter is the requirements-scope check
// (NVIDIA Ampere RTX by NVAPI) and records nothing.
bool ObserveVulkanPhysicalDevice(void*) noexcept;
bool ObserveVulkanAdapter(void*) noexcept;
struct PreparationBoundary
{
    uint64_t providerGeneration = 0;
    uint64_t freshLoadToken = 0;
    bool beforeFirstPipelineCreate = false;
    bool earlyInitializationProven = false;
    bool (*stillCurrent)(HMODULE, const PreparationBoundary&) noexcept = nullptr;
    constexpr bool Proven() const noexcept
    {
        return providerGeneration && freshLoadToken
            && beforeFirstPipelineCreate && earlyInitializationProven && stillCurrent;
    }
    bool Current(HMODULE provider) const noexcept
    { return Proven() && stillCurrent(provider, *this); }
};
// An empty boundary permits revalidation only. The host resolves fresh-load
// and first-init evidence while serializing preparation against feature Create.
bool PatchProvider(HMODULE, const wchar_t*, const PreparationBoundary& = {}) noexcept;
bool PreparedProvider(HMODULE) noexcept;
bool BindProvider(HMODULE) noexcept;
bool AdapterVerified() noexcept;
// Selected only after CUDA LUID and NVAPI TU102/104/106 identity agree.
bool IsTuring() noexcept;
bool Ready() noexcept;
uint32_t FailureCode() noexcept;
uint32_t PreparationStage() noexcept;
uint32_t KernelImage() noexcept;
uint32_t NativeCacheStatus() noexcept;
uint64_t AdapterLuid() noexcept;
uint64_t Publication() noexcept;
HMODULE Provider() noexcept;
}
