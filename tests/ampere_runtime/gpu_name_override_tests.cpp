// GPU name override (gpuname.1): the production module with MinHook detours on
// the real System32 dxgi.dll adapter methods. Needs an NVIDIA DXGI hardware
// adapter; on a GPU that is not RTX 30, testing::SetEligibleToken matches this
// adapter's own name so the reported identity can be checked. The scope checks
// call the same adapters from GpuNameProbe.dll (another module). All detours
// stay inside this test process.
#include "gpu_name_override.h"
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <string>
#include <vector>

unsigned checks = 0, failures = 0;
void Check(bool ok, const char* text)
{ ++checks; failures += !ok; std::cout << (ok ? "PASS " : "FAIL ") << text << '\n'; }

std::mutex gLogLock;
std::vector<std::wstring> gLog;
namespace single_module
{
bool OwnsBackend() noexcept { return true; }
void Log(const wchar_t* text) noexcept
{
    std::lock_guard lock(gLogLock);
    gLog.emplace_back(text ? text : L"");
}
}

namespace
{
using Microsoft::WRL::ComPtr;
using gpu_name_override::Setting;

size_t LogLines(const wchar_t* fragment)
{
    std::lock_guard lock(gLogLock);
    size_t count = 0;
    for (const auto& line : gLog) count += line.find(fragment) != std::wstring::npos;
    return count;
}

template<class Desc> bool SameExceptIdentity(const Desc& real, const Desc& seen)
{
    return seen.VendorId == real.VendorId && seen.SubSysId == real.SubSysId && seen.Revision == real.Revision
        && seen.DedicatedVideoMemory == real.DedicatedVideoMemory
        && seen.DedicatedSystemMemory == real.DedicatedSystemMemory
        && seen.SharedSystemMemory == real.SharedSystemMemory
        && !memcmp(&seen.AdapterLuid, &real.AdapterLuid, sizeof(LUID));
}
template<class Desc> bool Reported(const Desc& real, const Desc& seen)
{
    return SameExceptIdentity(real, seen) && seen.DeviceId == gpu_name_override::kReportedDeviceId
        && !wcscmp(seen.Description, gpu_name_override::kReportedName);
}
template<class Desc> bool Real(const Desc& real, const Desc& seen)
{
    return SameExceptIdentity(real, seen) && seen.DeviceId == real.DeviceId && !wcscmp(seen.Description, real.Description);
}

Setting SettingFrom(const std::filesystem::path& directory, const char* body)
{
    const auto path = directory / L"RTX40MFG-UI.ini";
    std::filesystem::remove(path);
    if (body) std::ofstream(path) << body;
    return gpu_name_override::ReadSetting(path.c_str());
}

struct Adapter
{
    ComPtr<IDXGIAdapter4> adapter;
    DXGI_ADAPTER_DESC desc{};
    DXGI_ADAPTER_DESC1 desc1{};
    DXGI_ADAPTER_DESC2 desc2{};
    DXGI_ADAPTER_DESC3 desc3{};
};
}

int wmain()
{
    // Name rule: NVIDIA plus an RTX 30 name, nothing else.
    using gpu_name_override::Eligible;
    Check(Eligible(0x10DE, L"NVIDIA GeForce RTX 3080"), "desktop RTX 3080 is eligible");
    Check(Eligible(0x10DE, L"NVIDIA GeForce RTX 3060 Laptop GPU"), "laptop RTX 3060 is eligible");
    Check(Eligible(0x10DE, L"NVIDIA GeForce RTX 3090 Ti"), "RTX 3090 Ti is eligible");
    Check(!Eligible(0x10DE, L"NVIDIA GeForce RTX 4080"), "RTX 40 is left alone");
    Check(!Eligible(0x10DE, L"NVIDIA GeForce RTX 5090"), "RTX 50 is left alone");
    Check(Eligible(0x10DE, L"NVIDIA GeForce RTX 2080 SUPER"), "RTX 20 uses the optional game name override");
    Check(!Eligible(0x10DE, L"NVIDIA GeForce GTX 1660 SUPER"), "GTX 16 is left alone");
    Check(!Eligible(0x10DE, L"NVIDIA RTX A4000"), "workstation Ampere is left alone");
    Check(!Eligible(0x10DE, L"NVIDIA A10G"), "data-centre Ampere is left alone");
    Check(!Eligible(0x1002, L"AMD Radeon RX 6800 (RTX 3080)"), "another vendor never qualifies");
    Check(!Eligible(0x10DE, nullptr), "a missing description never qualifies");
    wchar_t unterminated[140];
    wmemset(unterminated, L'x', 140);
    wmemcpy(unterminated + 128, L"RTX 30", 6);
    Check(!Eligible(0x10DE, unterminated), "the rule reads no further than 128 characters");
    wmemcpy(unterminated + 100, L"RTX 30", 6);
    Check(Eligible(0x10DE, unterminated), "an unterminated description is bounded, not rejected");

    // Apply: name and device ID only.
    DXGI_ADAPTER_DESC3 applied{};
    wcscpy_s(applied.Description, L"NVIDIA GeForce RTX 3070 Ti Laptop GPU with a long tail");
    applied.VendorId = 0x10DE; applied.DeviceId = 0x24A0; applied.SubSysId = 0x12345678; applied.Revision = 0xA1;
    applied.DedicatedVideoMemory = 8ull << 30; applied.SharedSystemMemory = 16ull << 30;
    applied.AdapterLuid.LowPart = 0xDF0B; applied.Flags = DXGI_ADAPTER_FLAG3_SUPPORT_MONITORED_FENCES;
    DXGI_ADAPTER_DESC3 before = applied;
    gpu_name_override::Apply(applied);
    Check(Reported(before, applied) && applied.Flags == before.Flags,
        "Apply changes only the name and device ID");
    bool zeroed = true;
    for (size_t i = wcslen(gpu_name_override::kReportedName); i < _countof(applied.Description); ++i)
        zeroed &= applied.Description[i] == L'\0';
    Check(zeroed, "no part of the real name survives after the reported one");

    // Setting parser on real ini files, beside this executable rather than in
    // %TEMP%: an executable under a folder with a Low mandatory label runs at
    // Low integrity and cannot create files in the user's %TEMP%.
    wchar_t testPath[MAX_PATH]{};
    GetModuleFileNameW(nullptr, testPath, MAX_PATH);
    const auto directory = std::filesystem::path(testPath).parent_path() / (L"gpuname-tests-" + std::to_wstring(GetCurrentProcessId()));
    std::error_code error;
    std::filesystem::create_directories(directory, error);
    if (error)
    {
        std::cout << "SETUP cannot create the ini folder beside the executable (error " << error.value() << ")\n";
        return 2;
    }
    Check(SettingFrom(directory, nullptr) == Setting::eOff, "no ini file means off");
    Check(SettingFrom(directory, "[Overlay]\nToggleKey=8\n") == Setting::eOff, "no key means off");
    Check(SettingFrom(directory, "[Compatibility]\nGpuNameOverride=1\n") == Setting::eOn, "1 is on");
    Check(SettingFrom(directory, "[Compatibility]\nGpuNameOverride = true \n") == Setting::eOn, "true is on");
    Check(SettingFrom(directory, "[compatibility]\ngpunameoverride=ON\n") == Setting::eOn, "names and values ignore case");
    Check(SettingFrom(directory, "[Compatibility]\nGpuNameOverride=0\n") == Setting::eOff, "0 is off");
    Check(SettingFrom(directory, "[Compatibility]\nGpuNameOverride=off\n") == Setting::eOff, "off is off");
    Check(SettingFrom(directory, "[Compatibility]\nGpuNameOverride=\n") == Setting::eOff, "an empty value is off");
    Check(SettingFrom(directory, "[Compatibility]\nGpuNameOverride=2\n") == Setting::eInvalid, "2 is not a setting");
    Check(SettingFrom(directory, "[Compatibility]\nGpuNameOverride=yes please\n") == Setting::eInvalid, "free text is not a setting");
    Check(SettingFrom(directory, "[Compatibility]\nGpuNameOverride=111111111111111111111\n") == Setting::eInvalid,
        "an overlong value is not a setting");
    Check(SettingFrom(directory, "[Other]\nGpuNameOverride=1\n") == Setting::eOff, "only the Compatibility section counts");
    Check(gpu_name_override::ReadSetting(nullptr) == Setting::eOff && gpu_name_override::ReadSetting(L"") == Setting::eOff,
        "no path means off");
    std::filesystem::remove_all(directory, error);

    // Menu toggle: the executable's own RTX40MFG-UI.ini, other settings kept.
    {
        wchar_t executable[MAX_PATH]{};
        GetModuleFileNameW(nullptr, executable, MAX_PATH);
        const auto ini = std::filesystem::path(executable).parent_path() / L"RTX40MFG-UI.ini";
        std::filesystem::remove(ini);
        Check(!gpu_name_override::SavedOn(), "nothing saved reads as off");
        std::ofstream(ini) << "[Overlay]\nToggleKey=8\n";
        Check(gpu_name_override::Save(true) && gpu_name_override::SavedOn(), "the menu saves on");
        Check(GetPrivateProfileIntW(L"Overlay", L"ToggleKey", 0, ini.c_str()) == 8, "saving keeps the other settings");
        Check(gpu_name_override::Save(false) && !gpu_name_override::SavedOn()
            && gpu_name_override::ReadSetting(ini.c_str()) == Setting::eOff, "the menu saves off as an explicit 0");
        Check(!gpu_name_override::Read().enabled, "saving never changes this session's state");
        std::filesystem::remove(ini);
    }

    // Real adapters, read before any detour exists.
    ComPtr<IDXGIFactory6> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))))
    {
        std::cout << "SETUP no DXGI factory\n";
        return 2;
    }
    std::vector<Adapter> adapters;
    for (UINT index = 0;; ++index)
    {
        ComPtr<IDXGIAdapter1> adapter1;
        if (factory->EnumAdapters1(index, &adapter1) == DXGI_ERROR_NOT_FOUND) break;
        Adapter entry;
        if (FAILED(adapter1.As(&entry.adapter))) continue;
        entry.adapter->GetDesc(&entry.desc);
        entry.adapter->GetDesc1(&entry.desc1);
        entry.adapter->GetDesc2(&entry.desc2);
        entry.adapter->GetDesc3(&entry.desc3);
        adapters.push_back(entry);
    }
    const Adapter* nvidia = nullptr;
    const Adapter* software = nullptr;
    for (const auto& entry : adapters)
    {
        if (entry.desc1.VendorId == 0x10DE && !nvidia) nvidia = &entry;
        if (entry.desc1.VendorId == 0x1414 && !software) software = &entry;
    }
    if (!nvidia)
    {
        std::cout << "SETUP no NVIDIA DXGI adapter\n";
        return 2;
    }
    wchar_t self[MAX_PATH]{};
    GetModuleFileNameW(nullptr, self, MAX_PATH);
    const HMODULE probeModule = LoadLibraryW((std::filesystem::path(self).parent_path() / L"GpuNameProbe.dll").c_str());
    using ProbeDesc1 = HRESULT(*)(IDXGIAdapter1*, DXGI_ADAPTER_DESC1*);
    using ProbeDesc = HRESULT(*)(IDXGIAdapter*, DXGI_ADAPTER_DESC*);
    using ProbeDesc3 = HRESULT(*)(IDXGIAdapter4*, DXGI_ADAPTER_DESC3*);
    const auto probeDesc = probeModule ? reinterpret_cast<ProbeDesc>(GetProcAddress(probeModule, "ProbeGetDesc")) : nullptr;
    const auto probeDesc1 = probeModule ? reinterpret_cast<ProbeDesc1>(GetProcAddress(probeModule, "ProbeGetDesc1")) : nullptr;
    const auto probeDesc3 = probeModule ? reinterpret_cast<ProbeDesc3>(GetProcAddress(probeModule, "ProbeGetDesc3")) : nullptr;
    if (!probeDesc || !probeDesc1 || !probeDesc3)
    {
        std::cout << "SETUP GpuNameProbe.dll missing\n";
        return 2;
    }
    std::wcout << L"INFO adapter '" << nvidia->desc1.Description << L"' device 0x" << std::hex << nvidia->desc1.DeviceId
        << std::dec << L"\n";

    const auto state0 = gpu_name_override::Read();
    Check(!state0.armed && !state0.methods && !state0.enabled, "nothing is armed before Arm");
    Check(gpu_name_override::Arm(), "Arm publishes the adapter detours");
    const auto armed = gpu_name_override::Read();
    Check(armed.armed && armed.methods == 0xF, "GetDesc, GetDesc1, GetDesc2 and GetDesc3 are all detoured");
    Check(!armed.enabled, "Arm alone does not claim the setting was read");
    Check(gpu_name_override::Arm() && gpu_name_override::Read().methods == 0xF, "a second Arm is a no-op");

    // Default rule on this GPU.
    const bool rtx30 = Eligible(nvidia->desc1.VendorId, nvidia->desc1.Description);
    DXGI_ADAPTER_DESC1 seen1{};
    Check(SUCCEEDED(nvidia->adapter->GetDesc1(&seen1)) && (rtx30 ? Reported(nvidia->desc1, seen1) : Real(nvidia->desc1, seen1)),
        rtx30 ? "this RTX 30 adapter reports the RTX 40 identity to the executable"
              : "this adapter is not RTX 30 and keeps its real identity");
    if (!rtx30)
    {
        Check(LogLines(L"left unchanged (only NVIDIA RTX 20/30 adapters") == 1, "the unchanged adapter is logged once");
        DXGI_ADAPTER_DESC again{};
        nvidia->adapter->GetDesc(&again);
        Check(LogLines(L"left unchanged") == 1, "repeated queries do not log again");
        Check(gpu_name_override::Read().unchanged >= 2 && !gpu_name_override::Read().reported, "unchanged queries are counted");
    }

    // The same detours with this adapter's own name as the rule.
    gpu_name_override::testing::SetEligibleToken(nvidia->desc1.Description);
    DXGI_ADAPTER_DESC seen{};
    DXGI_ADAPTER_DESC2 seen2{};
    DXGI_ADAPTER_DESC3 seen3{};
    Check(SUCCEEDED(nvidia->adapter->GetDesc(&seen)) && Reported(nvidia->desc, seen), "executable GetDesc reports the RTX 40 identity");
    Check(SUCCEEDED(nvidia->adapter->GetDesc1(&seen1)) && Reported(nvidia->desc1, seen1) && seen1.Flags == nvidia->desc1.Flags,
        "executable GetDesc1 reports the RTX 40 identity");
    Check(SUCCEEDED(nvidia->adapter->GetDesc2(&seen2)) && Reported(nvidia->desc2, seen2)
        && seen2.GraphicsPreemptionGranularity == nvidia->desc2.GraphicsPreemptionGranularity,
        "executable GetDesc2 reports the RTX 40 identity");
    Check(SUCCEEDED(nvidia->adapter->GetDesc3(&seen3)) && Reported(nvidia->desc3, seen3) && seen3.Flags == nvidia->desc3.Flags,
        "executable GetDesc3 reports the RTX 40 identity");
    Check(LogLines(L"reported as 'NVIDIA GeForce RTX 4090' (0x2684); caller=main-executable+0x") == 4,
        "each method logs its first report once");

    // Every adapter object of the class is covered, from any factory or route.
    ComPtr<IDXGIFactory6> second;
    ComPtr<IDXGIAdapter1> byLuid, byPreference;
    DXGI_ADAPTER_DESC1 viaLuid{}, viaPreference{};
    Check(SUCCEEDED(CreateDXGIFactory2(0, IID_PPV_ARGS(&second)))
        && SUCCEEDED(second->EnumAdapterByLuid(nvidia->desc1.AdapterLuid, IID_PPV_ARGS(&byLuid)))
        && SUCCEEDED(byLuid->GetDesc1(&viaLuid)) && Reported(nvidia->desc1, viaLuid),
        "an adapter from another factory by LUID reports the RTX 40 identity");
    if (SUCCEEDED(second->EnumAdapterByGpuPreference(0, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&byPreference)))
        && SUCCEEDED(byPreference->GetDesc1(&viaPreference))
        && !memcmp(&viaPreference.AdapterLuid, &nvidia->desc1.AdapterLuid, sizeof(LUID)))
        Check(Reported(nvidia->desc1, viaPreference), "the high-performance adapter route is covered too");

    // Other modules keep the real GPU.
    DXGI_ADAPTER_DESC other{};
    DXGI_ADAPTER_DESC1 other1{};
    DXGI_ADAPTER_DESC3 other3{};
    ComPtr<IDXGIAdapter> base;
    nvidia->adapter.As(&base);
    Check(SUCCEEDED(probeDesc(base.Get(), &other)) && Real(nvidia->desc, other), "another module's GetDesc sees the real GPU");
    Check(SUCCEEDED(probeDesc1(nvidia->adapter.Get(), &other1)) && Real(nvidia->desc1, other1),
        "another module's GetDesc1 sees the real GPU");
    Check(SUCCEEDED(probeDesc3(nvidia->adapter.Get(), &other3)) && Real(nvidia->desc3, other3),
        "another module's GetDesc3 sees the real GPU");

    // Microsoft's software adapter is never renamed, even when the rule would match it.
    if (software)
    {
        gpu_name_override::testing::SetEligibleToken(L"Microsoft");
        DXGI_ADAPTER_DESC1 warp{};
        const auto unchangedBefore = gpu_name_override::Read().unchanged;
        Check(SUCCEEDED(software->adapter->GetDesc1(&warp)) && Real(software->desc1, warp),
            "the software adapter keeps its real identity (vendor rule)");
        Check(gpu_name_override::Read().unchanged == unchangedBefore,
            "the software adapter is not counted as an unchanged GPU");
    }

    // Failures pass through unchanged.
    gpu_name_override::testing::SetEligibleToken(nvidia->desc1.Description);
    const auto reportedBefore = gpu_name_override::Read().reported;
    Check(FAILED(nvidia->adapter->GetDesc1(nullptr)) && gpu_name_override::Read().reported == reportedBefore,
        "a failed call passes through untouched");
    Check(reportedBefore >= 5, "reports are counted");

    std::cout << checks << " checks, " << failures << " failures\n";
    return failures ? 1 : 0;
}
