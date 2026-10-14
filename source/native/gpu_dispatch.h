#pragma once
#include "midpoint_fix.h"

namespace gpu_dispatch
{
enum class Family : uint32_t { eUnknown = 0, eAda = 1, eAmpere = 2, eConflict = 3 };
Family Selected() noexcept;
bool IsAda() noexcept;
bool IsAmpere() noexcept;
uint64_t AdapterLuid() noexcept;
// PCI vendor of the last observed game device (0 until a device is seen).
// Non-NVIDIA devices never reach the CUDA/NVAPI family probes.
uint32_t AdapterVendor() noexcept;
void SetLogCallback(midpoint_fix::LogCallback) noexcept;
bool ObserveD3D12Device(void*) noexcept;
bool ObserveVulkanPhysicalDevice(void*) noexcept;
bool PatchProvider(HMODULE, const wchar_t*) noexcept;
bool AdapterVerified() noexcept;
bool Ready() noexcept;
uint32_t FailureCode() noexcept;
}
