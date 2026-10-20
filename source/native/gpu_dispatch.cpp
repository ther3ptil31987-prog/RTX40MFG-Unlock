#include "gpu_dispatch.h"
#include "fault_capture.h"
#include "gpu_selection_policy.h"
#include "ampere_backend.h"
#include "ampere_provider_arch.h"
#include "blackwell_gpu.h"
#include "dlssg_provider_policy.h"
#include "adapter_vendor_kmt.h"
#include "single_module.h"
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <atomic>
#include <mutex>

namespace gpu_dispatch
{
namespace
{
std::mutex gSelectionMutex;
Selection gSelection;
std::atomic<Family> gFamily{Family::eUnknown};
std::atomic<uint64_t> gLuid{0};
std::atomic<midpoint_fix::LogCallback> gLog{nullptr};
std::atomic<uint32_t> gVendor{0};
uint64_t gVendorLuid = 0; // guarded by gSelectionMutex
uint32_t gVendorCached = 0;
constexpr uint32_t kNvidiaVendor = 0x10de;

// DXGI's description of the adapter that owns this LUID. Zero when DXGI cannot
// identify it; the family probes then keep their existing fail-closed checks.
uint32_t VendorForLuid(uint64_t luid) noexcept
{
    if (!luid) return 0;
    if (luid == gVendorLuid) return gVendorCached;
    uint32_t vendor = 0;
    try
    {
        Microsoft::WRL::ComPtr<IDXGIFactory4> factory;
        Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
        LUID identity{};
        identity.LowPart = static_cast<DWORD>(luid);
        identity.HighPart = static_cast<LONG>(luid >> 32);
        DXGI_ADAPTER_DESC1 description{};
        if (SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))
            && SUCCEEDED(factory->EnumAdapterByLuid(identity, IID_PPV_ARGS(&adapter)))
            && SUCCEEDED(adapter->GetDesc1(&description)))
            vendor = description.VendorId;
        // DXGI descriptions can be spoofed by another mod (OptiScaler reports
        // NVIDIA on every vendor by default); the kernel IDs for the LUID win.
        adapter_vendor_kmt::Ids ids{};
        if (adapter_vendor_kmt::Query(identity, ids) && ids.vendor != vendor)
        {
            wchar_t corrected[160]{};
            swprintf_s(corrected, L"GPU vendor: DXGI reported %04X, kernel adapter %04X; using the kernel value\n",
                vendor, ids.vendor);
            single_module::Log(corrected);
            vendor = ids.vendor;
        }
    }
    catch (...) { vendor = 0; }
    gVendorLuid = luid;
    gVendorCached = vendor;
    if (vendor && vendor != kNvidiaVendor)
    {
        wchar_t message[160]{};
        swprintf_s(message, L"Unified GPU selection: vendor=0x%04X adapterLuid=0x%016llX is not NVIDIA; "
            L"DLSS-G stays dormant (XeSS-FG remains available)", vendor, luid);
        if (auto log = gLog.load(std::memory_order_acquire)) log(message);
    }
    return vendor;
}

bool Publish(Family candidate, uint64_t luid) noexcept
{
    const Family previous = gSelection.family;
    const bool accepted = gSelection.Observe(candidate, luid);
    gLuid.store(gSelection.luid, std::memory_order_release);
    gFamily.store(gSelection.family, std::memory_order_release);
    if (previous != gSelection.family)
    {
        wchar_t message[192]{};
        swprintf_s(message, L"Unified GPU selection: family=%s adapterLuid=0x%016llX",
            gSelection.family == Family::eAda ? L"Ada (SM89)"
            : gSelection.family == Family::eBlackwell ? L"Blackwell RTX (SM120, native MFG, no patches)"
            : gSelection.family == Family::eTuring ? L"Turing RTX (SM75)"
            : gSelection.family == Family::eAmpere ? L"Ampere (SM86)"
            : L"conflict; restart required", gSelection.luid);
        if (auto log = gLog.load(std::memory_order_acquire)) log(message);
    }
    return accepted;
}
bool DeviceIdentity(void* source, ID3D12Device*& device, uint64_t& luid) noexcept
{
    device = nullptr;
    luid = 0;
    if (!source) return false;
    __try
    {
        if (FAILED(static_cast<IUnknown*>(source)->QueryInterface(__uuidof(ID3D12Device),
                reinterpret_cast<void**>(&device))) || !device) return false;
        const LUID identity = device->GetAdapterLuid();
        luid = static_cast<uint64_t>(identity.LowPart)
            | (static_cast<uint64_t>(static_cast<uint32_t>(identity.HighPart)) << 32);
        return luid != 0;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
}

Family Selected() noexcept { return gFamily.load(std::memory_order_acquire); }
bool IsAda() noexcept { return Selected() == Family::eAda; }
bool IsAmpere() noexcept { return Selected() == Family::eAmpere; }
bool IsTuring() noexcept { return Selected() == Family::eTuring; }
bool IsBlackwell() noexcept { return Selected() == Family::eBlackwell; }
bool UsesTranslatedProgram() noexcept { return IsAmpere() || IsTuring(); }
bool UsesNativeProgram() noexcept { return IsAda() || IsBlackwell(); }
uint64_t AdapterLuid() noexcept { return gLuid.load(std::memory_order_acquire); }
uint32_t AdapterVendor() noexcept { return gVendor.load(std::memory_order_acquire); }
void SetLogCallback(midpoint_fix::LogCallback callback) noexcept
{
    gLog.store(callback, std::memory_order_release);
    midpoint_fix::SetLogCallback(callback);
    blackwell_gpu::SetLogCallback(callback);
    ampere_backend::SetLogCallback(callback);
}
bool ObserveD3D12Device(void* source) noexcept
{
    fault_capture::ObserveDevice(source);
    std::lock_guard lock(gSelectionMutex);
    ID3D12Device* device = nullptr;
    uint64_t luid = 0;
    const bool identity = DeviceIdentity(source, device, luid);
    if (!identity || !gSelection.Allows(luid))
    {
        if (device) device->Release();
        return Publish(Family::eUnknown, luid);
    }
    const uint32_t vendor = VendorForLuid(luid);
    gVendor.store(vendor, std::memory_order_release);
    if (vendor && vendor != kNvidiaVendor)
    {
        // AMD, Intel and other GPUs: no cuInit, no NVAPI family probe.
        device->Release();
        return Publish(Family::eUnknown, luid);
    }
    Family candidate = Family::eUnknown;
    // A selected RTX 50 adapter is only re-identified: it has no RTX 40 or
    // RTX 30 program, and an unselected GPU tries RTX 40 first, as before.
    if (IsBlackwell())
        candidate = blackwell_gpu::ObserveD3D12Device(device) ? Family::eBlackwell : Family::eUnknown;
    else if (!UsesTranslatedProgram() && midpoint_fix::ObserveD3D12Device(device))
        candidate = Family::eAda;
    else if (gSelection.family == Family::eUnknown && blackwell_gpu::ObserveD3D12Device(device))
        candidate = Family::eBlackwell;
    else if (gSelection.family != Family::eAda && ampere_backend::ObserveD3D12Device(device))
        candidate = ampere_gpu::IsTuring() ? Family::eTuring : Family::eAmpere;
    device->Release();
    return Publish(candidate, luid);
}
bool ObserveNativeD3D12Device(void* source) noexcept
{
    std::lock_guard lock(gSelectionMutex);
    if (gSelection.family != Family::eUnknown)
        return gSelection.family == Family::eAda || gSelection.family == Family::eBlackwell;
    ID3D12Device* device = nullptr;
    uint64_t luid = 0;
    if (!DeviceIdentity(source, device, luid) || !gSelection.Allows(luid))
    {
        if (device) device->Release();
        return false;
    }
    const uint32_t vendor = VendorForLuid(luid);
    // Only a verified RTX 40 or RTX 50 adapter is published here; anything else
    // leaves the selection to the game's own device and Create boundaries.
    const bool nvidia = !vendor || vendor == kNvidiaVendor;
    const Family candidate = !nvidia ? Family::eUnknown
        : midpoint_fix::ObserveD3D12Device(device) ? Family::eAda
        : blackwell_gpu::ObserveD3D12Device(device) ? Family::eBlackwell : Family::eUnknown;
    device->Release();
    if (candidate == Family::eUnknown) return false;
    fault_capture::ObserveDevice(source);
    gVendor.store(vendor, std::memory_order_release);
    return Publish(candidate, luid);
}
bool ObserveVulkanPhysicalDevice(void* device) noexcept
{
    std::lock_guard lock(gSelectionMutex);
    if (gSelection.family == Family::eConflict)
        return false;
    if (IsBlackwell())
        return blackwell_gpu::ObserveVulkanPhysicalDevice(device)
            ? Publish(Family::eBlackwell, blackwell_gpu::AdapterLuid()) : Publish(Family::eUnknown, 0);
    if (!UsesTranslatedProgram())
    {
        if (midpoint_fix::ObserveVulkanPhysicalDevice(device))
            return Publish(Family::eAda, midpoint_fix::AdapterLuid());
        if (gSelection.family == Family::eUnknown && blackwell_gpu::ObserveVulkanPhysicalDevice(device))
            return Publish(Family::eBlackwell, blackwell_gpu::AdapterLuid());
        // No RTX 30 in this system (NVAPI): the Ada-only selection is unchanged.
        if (!ampere_provider_arch::Armed())
            return Publish(Family::eUnknown, 0);
    }
    // The VkPhysicalDevice LUID goes through the same CUDA compute-capability
    // verification as an RTX 30 D3D12 device.
    if (gSelection.family != Family::eAda && ampere_backend::ObserveVulkanPhysicalDevice(device))
        return Publish(ampere_gpu::IsTuring() ? Family::eTuring : Family::eAmpere, ampere_gpu::AdapterLuid());
    return Publish(Family::eUnknown, 0);
}
bool AdapterVerified() noexcept
{
    return IsAda() ? midpoint_fix::AdapterVerified()
        : IsBlackwell() ? blackwell_gpu::AdapterVerified()
        : UsesTranslatedProgram() && ampere_backend::AdapterVerified();
}
bool Ready() noexcept
{
    // RTX 50 has nothing to prepare: its provider runs unmodified.
    return IsAda() ? midpoint_fix::Ready()
        : IsBlackwell() ? blackwell_gpu::AdapterVerified()
        : UsesTranslatedProgram() && ampere_backend::Ready();
}
bool PatchProvider(HMODULE module, const wchar_t* path) noexcept
{
    // RTX 50 patches nothing; it only admits a supported, retained provider.
    return IsAda() ? midpoint_fix::PatchProvider(module, path)
        : IsBlackwell() ? blackwell_gpu::AdapterVerified() && dlssg_provider_policy::IsSupportedRetainedProvider(module)
        : UsesTranslatedProgram() && ampere_backend::PatchProvider(module, path);
}
uint32_t FailureCode() noexcept
{
    return IsAda() ? midpoint_fix::FailureCode()
        : IsBlackwell() ? blackwell_gpu::FailureCode()
        : UsesTranslatedProgram() ? ampere_backend::FailureCode()
        : Selected() == Family::eConflict ? 201u : 200u;
}
}
