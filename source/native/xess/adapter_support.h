#pragma once
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <stdexcept>
#include "../adapter_vendor_kmt.h"

namespace adapter_support
{
// The portable XeSS MFG route is admitted by capability, not by GPU family.
// Intel adapters keep Intel's own provider (Arc has native multi-frame
// generation); every other hardware vendor uses the same portable DXIL set.
enum class Route { Nvidia, Amd, Other };
inline const char* RouteName(Route route) noexcept
{
    switch (route)
    {
    case Route::Nvidia: return "nvidia_dxil";
    case Route::Amd: return "amd_dxil";
    case Route::Other: return "portable_dxil";
    default: return "unknown";
    }
}
constexpr UINT kIntelVendor = 0x8086;
constexpr UINT kNvidiaVendor = 0x10de;
constexpr UINT kAmdVendor = 0x1002;
struct Adapter
{
    DXGI_ADAPTER_DESC1 description{};
    LARGE_INTEGER driver{};
    Route route{};
    bool kernelIdsCorrected{};
};

inline bool SameLuid(LUID a, LUID b) noexcept
{ return a.LowPart == b.LowPart && a.HighPart == b.HighPart; }

// Replace DXGI's PCI IDs with the kernel adapter's when they differ. Another
// mod (OptiScaler DxgiSpoofing) can rewrite GetDesc* process-wide; the kernel
// IDs for the same LUID cannot be spoofed that way. Returns true on a correction.
inline bool CorrectSpoofedIds(DXGI_ADAPTER_DESC1& description) noexcept
{
    adapter_vendor_kmt::Ids ids{};
    if (!adapter_vendor_kmt::Query(description.AdapterLuid, ids)) return false;
    if (ids.vendor == description.VendorId && ids.device == description.DeviceId) return false;
    description.VendorId = ids.vendor;
    description.DeviceId = ids.device;
    description.SubSysId = ids.subSystem << 16 | ids.subVendor;
    description.Revision = ids.revision;
    return true;
}

// PCI vendor of the device's adapter, or 0 when DXGI cannot identify it.
inline UINT VendorOf(ID3D12Device* device) noexcept
{
    using Microsoft::WRL::ComPtr;
    if (!device) return 0;
    ComPtr<IDXGIFactory4> factory;
    ComPtr<IDXGIAdapter1> adapter;
    DXGI_ADAPTER_DESC1 description{};
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))
        || FAILED(factory->EnumAdapterByLuid(device->GetAdapterLuid(), IID_PPV_ARGS(&adapter)))
        || FAILED(adapter->GetDesc1(&description))) return 0;
    CorrectSpoofedIds(description);
    return description.VendorId;
}

// Query the authoritative device, not the first enumerated/display adapter.
// The template also lets the regression harness exercise this exact boundary.
template<class Device>
Route Qualify(Device* device, const DXGI_ADAPTER_DESC1& description)
{
    if (!device || device->GetNodeCount() != 1)
        throw std::runtime_error("unsupported_d3d12_device");
    const auto luid = device->GetAdapterLuid();
    if ((!luid.LowPart && !luid.HighPart) || !SameLuid(luid, description.AdapterLuid))
        throw std::runtime_error("d3d12_dxgi_luid_mismatch");
    if (description.Flags & (DXGI_ADAPTER_FLAG_SOFTWARE | DXGI_ADAPTER_FLAG_REMOTE))
        throw std::runtime_error("software_or_remote_adapter");
    if (description.VendorId == kIntelVendor)
        throw std::runtime_error("intel_adapter_uses_native_provider");
    D3D12_FEATURE_DATA_SHADER_MODEL shader{D3D_SHADER_MODEL_6_4};
    if (FAILED(device->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL, &shader, sizeof(shader)))
        || shader.HighestShaderModel < D3D_SHADER_MODEL_6_4)
        throw std::runtime_error("portable_programs_require_sm64");
    if (description.VendorId == kNvidiaVendor) return Route::Nvidia;
    if (description.VendorId == kAmdVendor) return Route::Amd;
    return Route::Other;
}

inline Adapter Inspect(ID3D12Device* device)
{
    using Microsoft::WRL::ComPtr;
    if (!device) throw std::runtime_error("unsupported_d3d12_device");
    ComPtr<IDXGIFactory4> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))))
        throw std::runtime_error("adapter_factory");
    ComPtr<IDXGIAdapter1> adapter;
    if (FAILED(factory->EnumAdapterByLuid(device->GetAdapterLuid(), IID_PPV_ARGS(&adapter))))
        throw std::runtime_error("authoritative_adapter");
    Adapter result;
    if (FAILED(adapter->GetDesc1(&result.description))) throw std::runtime_error("adapter_description");
    result.kernelIdsCorrected = CorrectSpoofedIds(result.description);
    result.route = Qualify(device, result.description);
    if (FAILED(adapter->CheckInterfaceSupport(__uuidof(IDXGIDevice), &result.driver)))
        throw std::runtime_error("driver_version");
    return result;
}
}
