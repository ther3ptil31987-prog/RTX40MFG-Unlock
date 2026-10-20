#pragma once
#include "midpoint_fix.h"

namespace gpu_dispatch
{
enum class Family : uint32_t { eUnknown = 0, eAda = 1, eAmpere = 2, eConflict = 3, eTuring = 4, eBlackwell = 5 };
Family Selected() noexcept;
bool IsAda() noexcept;
bool IsAmpere() noexcept;
bool IsTuring() noexcept;
bool IsBlackwell() noexcept;
// RTX 20/30 share the lifetime/capacity backend, with separate GPU programs.
bool UsesTranslatedProgram() noexcept;
// RTX 40 and RTX 50: the DLSS-G provider runs its own program. RTX 40 needs
// the provider and wrapper patches for it; RTX 50 runs it unmodified.
bool UsesNativeProgram() noexcept;
uint64_t AdapterLuid() noexcept;
// PCI vendor of the last observed game device (0 until a device is seen).
// Non-NVIDIA devices never reach the CUDA/NVAPI family probes.
uint32_t AdapterVendor() noexcept;
void SetLogCallback(midpoint_fix::LogCallback) noexcept;
bool ObserveD3D12Device(void*) noexcept;
// Selects RTX 40 or RTX 50 only (a verified adapter of either); never selects
// RTX 30 and publishes nothing for other adapters. For devices seen before the
// game's own selection boundaries, such as OptiScaler's private Streamline.
bool ObserveNativeD3D12Device(void*) noexcept;
bool ObserveVulkanPhysicalDevice(void*) noexcept;
bool PatchProvider(HMODULE, const wchar_t*) noexcept;
bool AdapterVerified() noexcept;
bool Ready() noexcept;
uint32_t FailureCode() noexcept;
}
