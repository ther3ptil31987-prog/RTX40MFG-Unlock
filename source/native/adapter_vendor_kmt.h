#pragma once
#include <Windows.h>
#include <cstddef>
#include <cstdint>

// Kernel-reported PCI IDs for a D3D adapter LUID. DXGI adapter descriptions
// can be rewritten by other mods (OptiScaler's DxgiSpoofing reports an RTX 4090,
// 0x10DE/0x2684, on every vendor by default); D3DKMT_QUERY_DEVICE_IDS comes from
// the kernel adapter object and is not part of that spoof. Layouts match the
// Windows SDK 10.0.22621 d3dkmthk.h definitions (checked below).
namespace adapter_vendor_kmt
{
struct Ids
{
    uint32_t vendor{};
    uint32_t device{};
    uint32_t subVendor{};
    uint32_t subSystem{};
    uint32_t revision{};
};

namespace detail
{
using Handle = UINT;
struct OpenFromLuid { LUID adapterLuid; Handle adapter; };
struct Close { Handle adapter; };
struct QueryInfo { Handle adapter; UINT type; void* data; UINT size; };
struct DeviceIds { UINT vendor, device, subVendor, subSystem, revision, busType; };
struct QueryDeviceIds { UINT physicalAdapterIndex; DeviceIds ids; };
static_assert(sizeof(OpenFromLuid) == 12 && offsetof(OpenFromLuid, adapter) == 8);
static_assert(sizeof(QueryInfo) == 24 && offsetof(QueryInfo, data) == 8 && offsetof(QueryInfo, size) == 16);
static_assert(sizeof(QueryDeviceIds) == 28);
constexpr UINT kPhysicalAdapterDeviceIds = 31;  // KMTQAITYPE_PHYSICALADAPTERDEVICEIDS
using OpenFn = LONG(APIENTRY*)(OpenFromLuid*);
using CloseFn = LONG(APIENTRY*)(const Close*);
using QueryFn = LONG(APIENTRY*)(const QueryInfo*);
}

// Never call under the loader lock (gdi32 may need to load).
inline bool Query(const LUID& luid, Ids& out) noexcept
{
    out = {};
    if (!luid.LowPart && !luid.HighPart) return false;
    HMODULE gdi = GetModuleHandleW(L"gdi32.dll");
    if (!gdi) gdi = LoadLibraryExW(L"gdi32.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!gdi) return false;
    const auto open = reinterpret_cast<detail::OpenFn>(GetProcAddress(gdi, "D3DKMTOpenAdapterFromLuid"));
    const auto close = reinterpret_cast<detail::CloseFn>(GetProcAddress(gdi, "D3DKMTCloseAdapter"));
    const auto query = reinterpret_cast<detail::QueryFn>(GetProcAddress(gdi, "D3DKMTQueryAdapterInfo"));
    if (!open || !close || !query) return false;
    detail::OpenFromLuid opened{luid, 0};
    if (open(&opened) < 0 || !opened.adapter) return false;
    detail::QueryDeviceIds ids{};
    ids.physicalAdapterIndex = 0;
    const detail::QueryInfo info{opened.adapter, detail::kPhysicalAdapterDeviceIds, &ids, sizeof(ids)};
    const LONG status = query(&info);
    const detail::Close closing{opened.adapter};
    close(&closing);
    if (status < 0 || !ids.ids.vendor) return false;
    out = {ids.ids.vendor, ids.ids.device, ids.ids.subVendor, ids.ids.subSystem, ids.ids.revision};
    return true;
}
}
