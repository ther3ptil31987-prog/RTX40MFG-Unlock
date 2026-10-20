// blackwell_gpu's real CUDA identification on this machine's NVIDIA adapters:
// every D3D12 hardware adapter must be identified exactly as CUDA reports its
// compute capability (12.x = RTX 50), and a rejected adapter is not queried
// again. Exit 2: no NVIDIA D3D12 adapter or no CUDA driver (setup failure).
#include "blackwell_gpu.h"

#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace
{
std::vector<std::wstring> gLog;
void Log(const wchar_t* message) noexcept
{
    try { gLog.emplace_back(message); } catch (...) {}
    std::wprintf(L"  log: %s\n", message);
}

// CUDA's own compute capability for this LUID, independent of the module.
bool CudaCapability(const LUID& luid, int& major, int& minor)
{
    using CuInit = int (WINAPI*)(unsigned int);
    using CuDeviceGetCount = int (WINAPI*)(int*);
    using CuDeviceGet = int (WINAPI*)(int*, int);
    using CuDeviceComputeCapability = int (WINAPI*)(int*, int*, int);
    using CuDeviceGetLuid = int (WINAPI*)(char*, unsigned int*, int);
    HMODULE cuda = LoadLibraryExW(L"nvcuda.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!cuda) return false;
    auto* init = reinterpret_cast<CuInit>(GetProcAddress(cuda, "cuInit"));
    auto* count = reinterpret_cast<CuDeviceGetCount>(GetProcAddress(cuda, "cuDeviceGetCount"));
    auto* get = reinterpret_cast<CuDeviceGet>(GetProcAddress(cuda, "cuDeviceGet"));
    auto* capability = reinterpret_cast<CuDeviceComputeCapability>(GetProcAddress(cuda, "cuDeviceComputeCapability"));
    auto* getLuid = reinterpret_cast<CuDeviceGetLuid>(GetProcAddress(cuda, "cuDeviceGetLuid"));
    int devices = 0;
    bool found = false;
    if (init && count && get && capability && getLuid && init(0) == 0 && count(&devices) == 0)
        for (int i = 0; i < devices && !found; ++i)
        {
            int device = 0;
            char raw[sizeof(LUID)]{};
            unsigned int mask = 0;
            if (get(&device, i) || getLuid(raw, &mask, device)) continue;
            LUID candidate{};
            std::memcpy(&candidate, raw, sizeof(candidate));
            found = candidate.LowPart == luid.LowPart && candidate.HighPart == luid.HighPart
                && capability(&major, &minor, device) == 0;
        }
    FreeLibrary(cuda);
    return found;
}
}

int main()
{
    blackwell_gpu::SetLogCallback(&Log);
    ComPtr<IDXGIFactory6> factory;
    if (FAILED(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory))))
    {
        std::printf("SETUP no DXGI factory\n");
        return 2;
    }
    unsigned checked = 0, failures = 0;
    for (UINT index = 0;; ++index)
    {
        ComPtr<IDXGIAdapter1> adapter;
        if (factory->EnumAdapters1(index, &adapter) == DXGI_ERROR_NOT_FOUND) break;
        DXGI_ADAPTER_DESC1 desc{};
        if (FAILED(adapter->GetDesc1(&desc)) || desc.VendorId != 0x10de || (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE))
            continue;
        int major = 0, minor = 0;
        if (!CudaCapability(desc.AdapterLuid, major, minor))
        {
            std::wprintf(L"SKIP %s: no CUDA device with this LUID\n", desc.Description);
            continue;
        }
        ComPtr<ID3D12Device> device;
        if (FAILED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device))))
        {
            std::wprintf(L"SKIP %s: no D3D12 device\n", desc.Description);
            continue;
        }
        ++checked;
        const bool expected = blackwell_gpu::IsBlackwellRtxCapability(major, minor);
        const bool first = blackwell_gpu::ObserveD3D12Device(device.Get());
        const size_t logged = gLog.size();
        const bool second = blackwell_gpu::ObserveD3D12Device(device.Get());
        const uint32_t failure = blackwell_gpu::FailureCode();
        const bool pass = first == expected && second == expected && gLog.size() == logged
            && (expected ? failure == 0 && blackwell_gpu::AdapterVerified()
                    : failure == static_cast<uint32_t>(blackwell_gpu::Failure::eAdapterNotBlackwell));
        std::wprintf(L"%s %s: CUDA %d.%d identified=%d repeat=%d failure=%u\n", pass ? L"PASS" : L"FAIL",
            desc.Description, major, minor, first, second, failure);
        if (!pass) ++failures;
        if (expected) break; // A verified adapter stays the process's adapter.
    }
    if (!checked)
    {
        std::printf("SETUP no NVIDIA D3D12 adapter with a CUDA device\n");
        return 2;
    }
    std::printf("COMPLETE Blackwell adapter probe adapters=%u failures=%u\n", checked, failures);
    return failures ? 1 : 0;
}
