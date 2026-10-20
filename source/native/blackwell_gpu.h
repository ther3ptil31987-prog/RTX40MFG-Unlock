#pragma once

#include <Windows.h>

#include <cstddef>
#include <cstdint>

// RTX 50 (Blackwell) runs DLSS Multi Frame Generation natively. This module
// only identifies that adapter. It publishes no provider program and patches
// nothing: the menu's controls run on the game's own Streamline and NGX calls,
// as on RTX 40, without RTX 40's provider and wrapper patches.
namespace blackwell_gpu
{
using LogCallback = void (*)(const wchar_t* message);

enum class Failure : uint32_t
{
    eNone = 0,
    eAdapterUnavailable = 1,
    eAdapterNotBlackwell = 2,
    eRestartRequired = 3,
};

struct CudaDevice
{
    LUID luid{};
    int major = 0;
    int minor = 0;
};

// CUDA compute capability 12.x: the Blackwell RTX GPUs (sm_120 family).
// Data-center Blackwell (10.x) has no DLSS Frame Generation.
constexpr bool IsBlackwellRtxCapability(int major, int minor) noexcept
{
    return major == 12 && minor >= 0;
}

// Exactly one CUDA device carries the adapter's LUID, and it is Blackwell RTX.
inline bool MatchAdapter(const LUID& active, const CudaDevice* devices, size_t count,
    int& major, int& minor) noexcept
{
    major = 0;
    minor = 0;
    if (!devices || !count || (active.LowPart == 0 && active.HighPart == 0)) return false;
    size_t matches = 0;
    for (size_t i = 0; i != count; ++i)
    {
        if (devices[i].luid.LowPart != active.LowPart || devices[i].luid.HighPart != active.HighPart)
            continue;
        ++matches;
        major = devices[i].major;
        minor = devices[i].minor;
    }
    return matches == 1 && IsBlackwellRtxCapability(major, minor);
}

void SetLogCallback(LogCallback callback) noexcept;
bool ObserveD3D12Device(void* device) noexcept;
bool ObserveVulkanPhysicalDevice(void* physicalDevice) noexcept;
uint64_t AdapterLuid() noexcept;
bool AdapterVerified() noexcept;
uint32_t FailureCode() noexcept;
}
