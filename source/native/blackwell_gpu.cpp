#include "blackwell_gpu.h"
#include "vulkan_physical_device.h"

#include <d3d12.h>

#include <array>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>

namespace blackwell_gpu
{
namespace
{
std::atomic<LogCallback> gLog{nullptr};
std::mutex gMutex;
std::atomic<uint64_t> gAdapterLuid{0};
std::atomic<bool> gAdapterVerified{false};
std::atomic<uint32_t> gFailure{static_cast<uint32_t>(Failure::eAdapterUnavailable)};
// The last result logged, so repeated device observations log once.
uint64_t gLoggedLuid = 0;
uint32_t gLoggedResult = 0;
// An adapter a complete CUDA enumeration found not to be Blackwell RTX. Device
// observations repeat thousands of times per session; it is not queried again.
uint64_t gRejectedLuid = 0;

uint64_t PackLuid(const LUID& luid) noexcept
{
    return static_cast<uint64_t>(luid.LowPart)
        | (static_cast<uint64_t>(static_cast<uint32_t>(luid.HighPart)) << 32);
}

void SetFailure(Failure failure) noexcept
{
    gFailure.store(static_cast<uint32_t>(failure), std::memory_order_release);
}

// The CUDA devices with their LUIDs and compute capabilities, through the
// driver's nvcuda.dll, as the RTX 40 and RTX 30 adapter checks do.
bool QueryCudaDevices(std::array<CudaDevice, 16>& devices, size_t& count) noexcept
{
    using CuInit = int (WINAPI*)(unsigned int);
    using CuDeviceGetCount = int (WINAPI*)(int*);
    using CuDeviceGet = int (WINAPI*)(int*, int);
    using CuDeviceComputeCapability = int (WINAPI*)(int*, int*, int);
    using CuDeviceGetLuid = int (WINAPI*)(char*, unsigned int*, int);

    count = 0;
    HMODULE cuda = LoadLibraryExW(L"nvcuda.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!cuda) return false;
    auto* initialize = reinterpret_cast<CuInit>(GetProcAddress(cuda, "cuInit"));
    auto* getCount = reinterpret_cast<CuDeviceGetCount>(GetProcAddress(cuda, "cuDeviceGetCount"));
    auto* getDevice = reinterpret_cast<CuDeviceGet>(GetProcAddress(cuda, "cuDeviceGet"));
    auto* getCapability = reinterpret_cast<CuDeviceComputeCapability>(
        GetProcAddress(cuda, "cuDeviceComputeCapability"));
    auto* getLuid = reinterpret_cast<CuDeviceGetLuid>(GetProcAddress(cuda, "cuDeviceGetLuid"));
    int reported = 0;
    bool complete = initialize && getCount && getDevice && getCapability && getLuid
        && initialize(0) == 0 && getCount(&reported) == 0 && reported > 0
        && static_cast<size_t>(reported) <= devices.size();
    for (int ordinal = 0; complete && ordinal < reported; ++ordinal)
    {
        int device = 0;
        CudaDevice& entry = devices[static_cast<size_t>(ordinal)];
        std::array<char, sizeof(LUID)> rawLuid{};
        unsigned int nodeMask = 0;
        if (getDevice(&device, ordinal) != 0
            || getCapability(&entry.major, &entry.minor, device) != 0
            || getLuid(rawLuid.data(), &nodeMask, device) != 0)
        {
            complete = false;
            break;
        }
        std::memcpy(&entry.luid, rawLuid.data(), sizeof(entry.luid));
    }
    FreeLibrary(cuda);
    if (complete) count = static_cast<size_t>(reported);
    return complete;
}

bool ObserveAdapterLuid(const LUID& luid, const wchar_t* api) noexcept
{
    const uint64_t packed = PackLuid(luid);
    {
        std::lock_guard lock(gMutex);
        // A verified adapter stays the process's adapter.
        if (gAdapterVerified.load(std::memory_order_acquire))
            return gAdapterLuid.load(std::memory_order_acquire) == packed;
        if (packed && packed == gRejectedLuid)
            return false;
    }
    std::array<CudaDevice, 16> devices{};
    size_t count = 0;
    int major = 0, minor = 0;
    const bool queried = QueryCudaDevices(devices, count);
    const bool verified = queried && MatchAdapter(luid, devices.data(), count, major, minor);
    bool report = false;
    {
        std::lock_guard lock(gMutex);
        if (gAdapterVerified.load(std::memory_order_acquire))
            return gAdapterLuid.load(std::memory_order_acquire) == packed;
        if (verified)
        {
            gAdapterLuid.store(packed, std::memory_order_release);
            gAdapterVerified.store(true, std::memory_order_release);
            SetFailure(Failure::eNone);
        }
        else
        {
            SetFailure(major == 0 && minor == 0 ? Failure::eAdapterUnavailable : Failure::eAdapterNotBlackwell);
            if (queried) gRejectedLuid = packed;
        }
        const uint32_t result = 0x80000000u | (static_cast<uint32_t>(major & 0xFF) << 16)
            | (static_cast<uint32_t>(minor & 0xFF) << 8) | (verified ? 1u : 0u);
        report = gLoggedLuid != packed || gLoggedResult != result;
        gLoggedLuid = packed;
        gLoggedResult = result;
    }
    if (report)
        if (const auto log = gLog.load(std::memory_order_acquire))
        {
            wchar_t message[192]{};
            swprintf_s(message, L"RTX 50 %s adapter verification: luid=0x%016llX capability=%d.%d verified=%d",
                api ? api : L"unknown", static_cast<unsigned long long>(packed), major, minor, verified);
            log(message);
        }
    return verified;
}
}

void SetLogCallback(LogCallback callback) noexcept
{
    gLog.store(callback, std::memory_order_release);
}

bool ObserveD3D12Device(void* device) noexcept
{
    if (!device) return false;
    ID3D12Device* d3d12 = nullptr;
    if (FAILED(static_cast<IUnknown*>(device)->QueryInterface(__uuidof(ID3D12Device),
            reinterpret_cast<void**>(&d3d12))) || !d3d12)
        return false;
    const LUID luid = d3d12->GetAdapterLuid();
    d3d12->Release();
    return ObserveAdapterLuid(luid, L"D3D12");
}

bool ObserveVulkanPhysicalDevice(void* physicalDevice) noexcept
{
    LUID luid{};
    if (!vulkan_physical_device::QueryLuid(physicalDevice, luid)) return false;
    return ObserveAdapterLuid(luid, L"Vulkan");
}

uint64_t AdapterLuid() noexcept { return gAdapterLuid.load(std::memory_order_acquire); }
bool AdapterVerified() noexcept { return gAdapterVerified.load(std::memory_order_acquire); }
uint32_t FailureCode() noexcept { return gFailure.load(std::memory_order_acquire); }
}
