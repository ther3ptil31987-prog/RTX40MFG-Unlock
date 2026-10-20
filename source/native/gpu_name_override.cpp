#include "gpu_name_override.h"
#include "overlay_native.h"
#include "process_mitigations.h"
#include "provider_image.h"
#include "single_module.h"
#include "ui_host.h"
#include <MinHook.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <intrin.h>
#include <array>
#include <atomic>
#include <cstring>
#include <mutex>

namespace gpu_name_override
{
namespace
{
using Microsoft::WRL::ComPtr;

struct MethodInfo { const wchar_t* name; size_t slot; };
// IDXGIAdapter::GetDesc, IDXGIAdapter1::GetDesc1, IDXGIAdapter2::GetDesc2 and
// IDXGIAdapter4::GetDesc3, each read from its own interface's table.
constexpr std::array<MethodInfo, 4> kMethods{{
    {L"GetDesc", 8}, {L"GetDesc1", 10}, {L"GetDesc2", 11}, {L"GetDesc3", 18}}};

// Trampolines have process lifetime; each is published before its detour.
std::array<std::atomic<void*>, 4> gOriginals{};
std::array<void*, 4> gTargets{}; // guarded by gArming
std::array<std::atomic<bool>, 4> gReportLogged{};
std::atomic<bool> gUnchangedLogged{false};
std::atomic<bool> gEnabled{false};
std::atomic<bool> gArmed{false};
std::atomic<uint32_t> gMethods{0};
std::atomic<uint64_t> gReported{0};
std::atomic<uint64_t> gUnchanged{0};
std::mutex gArming;

#if MFG_GPU_NAME_OVERRIDE_TESTING
std::atomic<const wchar_t*> gToken{L"RTX 30"};
const wchar_t* Token() noexcept { return gToken.load(std::memory_order_acquire); }
#else
constexpr const wchar_t* Token() noexcept { return L"RTX 30"; }
#endif

// Milliseconds since process creation: orders the arming against the game's
// first query in a log.
unsigned long long ProcessAgeMs() noexcept
{
    FILETIME created{}, exited{}, kernel{}, user{}, now{};
    if (!GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user)) return 0;
    GetSystemTimeAsFileTime(&now);
    const auto ticks = [](const FILETIME& time) {
        return (static_cast<unsigned long long>(time.dwHighDateTime) << 32) | time.dwLowDateTime; };
    return ticks(now) > ticks(created) ? (ticks(now) - ticks(created)) / 10000 : 0;
}

// The game's own code: an executable image page of the main module. Streamline,
// NGX, the driver, middleware and this DLL never qualify. Returns offset + 1.
uintptr_t GameCaller(void* caller) noexcept
{
    static const HMODULE executable = GetModuleHandleW(nullptr);
    if (!single_overlay::slots::ImageEntry(executable, caller)) return 0;
    return reinterpret_cast<uintptr_t>(caller) - reinterpret_cast<uintptr_t>(executable) + 1;
}

template<class Desc> void Report(size_t method, Desc& desc, uintptr_t caller) noexcept
{
    if (!Eligible(desc.VendorId, desc.Description))
    {
        // Microsoft's software adapter is always enumerated; it says nothing about the GPU.
        if (desc.VendorId == 0x1414) return;
        ++gUnchanged;
        if (gUnchangedLogged.exchange(true, std::memory_order_acq_rel)) return;
        wchar_t line[512]{};
        swprintf_s(line, L"GPU name override: game %s for '%.128s' vendor=0x%04X left unchanged "
            L"(only NVIDIA RTX 20/30 adapters are reported as RTX 40)",
            kMethods[method].name, desc.Description, desc.VendorId);
        single_module::Log(line);
        return;
    }
    wchar_t real[129]{};
    wmemcpy(real, desc.Description, wcsnlen(desc.Description, _countof(desc.Description)));
    const UINT realDevice = desc.DeviceId;
    Apply(desc);
    ++gReported;
    if (gReportLogged[method].exchange(true, std::memory_order_acq_rel)) return;
    wchar_t line[512]{};
    swprintf_s(line, L"GPU name override: game %s for '%s' (0x%04X) reported as '%s' (0x%04X); "
        L"caller=main-executable+0x%llX luid=0x%08lX%08lX processAgeMs=%llu",
        kMethods[method].name, real, realDevice, desc.Description, desc.DeviceId,
        static_cast<unsigned long long>(caller - 1),
        static_cast<unsigned long>(desc.AdapterLuid.HighPart), desc.AdapterLuid.LowPart, ProcessAgeMs());
    single_module::Log(line);
}

template<size_t I, class Adapter, class Desc>
HRESULT STDMETHODCALLTYPE Describe(Adapter* adapter, Desc* desc)
{
    using Fn = HRESULT(STDMETHODCALLTYPE*)(Adapter*, Desc*);
    const auto original = reinterpret_cast<Fn>(gOriginals[I].load(std::memory_order_acquire));
    if (!original) return E_UNEXPECTED; // Never published before its trampoline.
    const HRESULT result = original(adapter, desc);
    if (FAILED(result) || !desc) return result;
    const DWORD error = GetLastError();
    // Every other caller keeps the real GPU.
    if (const uintptr_t caller = GameCaller(_ReturnAddress()))
        Report(I, *desc, caller);
    SetLastError(error);
    return result;
}

const std::array<void*, 4> kDetours{
    reinterpret_cast<void*>(&Describe<0, IDXGIAdapter, DXGI_ADAPTER_DESC>),
    reinterpret_cast<void*>(&Describe<1, IDXGIAdapter1, DXGI_ADAPTER_DESC1>),
    reinterpret_cast<void*>(&Describe<2, IDXGIAdapter2, DXGI_ADAPTER_DESC2>),
    reinterpret_cast<void*>(&Describe<3, IDXGIAdapter4, DXGI_ADAPTER_DESC3>)};

// The interface table and the method must both belong to System32 dxgi.dll
// (a local proxy dxgi.dll is never detoured).
void* SystemMethod(IUnknown* object, size_t slot, HMODULE dxgi) noexcept
{
    if (!object || !single_overlay::native::SystemDxgiObject(object, slot)) return nullptr;
    void* target = (*reinterpret_cast<void***>(object))[slot];
    return single_overlay::slots::ImageEntry(dxgi, target) ? target : nullptr;
}

// Caller holds gArming.
bool Detour(size_t index, void* target) noexcept
{
    for (size_t other = 0; other < gTargets.size(); ++other)
        if (gTargets[other] == target) return false; // One callable never serves two layouts.
    void* trampoline = nullptr;
    if (MH_CreateHook(target, kDetours[index], &trampoline) != MH_OK || !trampoline) return false;
    gTargets[index] = target;
    gOriginals[index].store(trampoline, std::memory_order_release);
    // Once enabled, the trampoline stays published even if verification fails,
    // so a concurrent caller always has a valid original.
    if (MH_EnableHook(target) != MH_OK) return false;
    // Accept only the decoded MinHook route to the expected detour.
    return provider_image::MinHookRelay(target, kDetours[index]) != nullptr;
}

bool ArmLocked(const wchar_t*& reason) noexcept
{
    const auto fail = [&](const wchar_t* text) { reason = text; return false; };
    if (gArmed.load(std::memory_order_acquire)) return true;
    if (single_overlay::native::InsideLoader()) return fail(L"loader lock");
    if (!HookMitigationsSupported()) return fail(L"strict CFG/CET policy");
    wchar_t path[MAX_PATH]{};
    if (!GetSystemDirectoryW(path, MAX_PATH) || wcscat_s(path, L"\\dxgi.dll")) return fail(L"System32 path");
    // The System32 image by full path: a local proxy dxgi.dll is never used here.
    const HMODULE dxgi = LoadLibraryW(path);
    using CreateFactory1 = HRESULT(WINAPI*)(REFIID, void**);
    const auto create = dxgi ? reinterpret_cast<CreateFactory1>(GetProcAddress(dxgi, "CreateDXGIFactory1")) : nullptr;
    if (!create) return fail(L"System32 dxgi.dll");
    ComPtr<IDXGIFactory1> factory;
    ComPtr<IDXGIAdapter1> adapter1;
    if (FAILED(create(IID_PPV_ARGS(&factory))) || !factory) return fail(L"DXGI factory");
    if (FAILED(factory->EnumAdapters1(0, &adapter1)) || !adapter1) return fail(L"no DXGI adapter");
    ComPtr<IDXGIAdapter> adapter;
    ComPtr<IDXGIAdapter2> adapter2;
    ComPtr<IDXGIAdapter4> adapter4;
    adapter1.As(&adapter);
    adapter1.As(&adapter2);
    adapter1.As(&adapter4);
    const std::array<IUnknown*, 4> objects{adapter.Get(), adapter1.Get(), adapter2.Get(), adapter4.Get()};
    const MH_STATUS initialized = MH_Initialize();
    if (initialized != MH_OK && initialized != MH_ERROR_ALREADY_INITIALIZED) return fail(L"MinHook");
    uint32_t methods = gMethods.load(std::memory_order_acquire);
    for (size_t i = 0; i < kMethods.size(); ++i)
    {
        if (methods & (1u << i)) continue;
        void* target = SystemMethod(objects[i], kMethods[i].slot, dxgi);
        if (target && Detour(i, target)) methods |= 1u << i;
    }
    gMethods.store(methods, std::memory_order_release);
    // GetDesc and GetDesc1 are what engines read; GetDesc2/3 exist from Windows 8.1/10.
    if ((methods & 0x3u) != 0x3u) return fail(L"adapter methods not published");
    gArmed.store(true, std::memory_order_release);
    return true;
}
}

Setting ReadSetting(const wchar_t* iniPath) noexcept { return standalone_ui::ReadOnOff(kSection, kKey, iniPath, Setting::eOff); }

bool Eligible(UINT vendorId, const wchar_t* description) noexcept
{
    if (vendorId != kNvidiaVendorId || !description) return false;
    // DXGI descriptions hold 128 characters; never read past them.
    wchar_t bounded[129]{};
    wmemcpy(bounded, description, wcsnlen(description, 128));
    return wcsstr(bounded, Token()) != nullptr || (!wcscmp(Token(), L"RTX 30") && wcsstr(bounded, L"RTX 20") != nullptr);
}

State Read() noexcept
{
    State state;
    state.enabled = gEnabled.load(std::memory_order_acquire);
    state.armed = gArmed.load(std::memory_order_acquire);
    state.methods = gMethods.load(std::memory_order_acquire);
    state.reported = gReported.load(std::memory_order_relaxed);
    state.unchanged = gUnchanged.load(std::memory_order_relaxed);
    return state;
}

bool Arm() noexcept
{
    std::lock_guard lock(gArming);
    const wchar_t* reason = L"";
    return ArmLocked(reason);
}

bool SavedOn() noexcept
{
    try { return ReadSetting(standalone_ui::SettingsPath("RTX40MFG-UI.ini").c_str()) == Setting::eOn; }
    catch (...) { return false; }
}

bool Save(bool on) noexcept
{
    try
    {
        const std::wstring path = standalone_ui::SettingsPath("RTX40MFG-UI.ini");
        return !path.empty() && WritePrivateProfileStringW(kSection, kKey, on ? L"1" : L"0", path.c_str())
            && ReadSetting(path.c_str()) == (on ? Setting::eOn : Setting::eOff);
    }
    catch (...) { return false; }
}

void ArmFromSettings() noexcept
{
    if (!single_module::OwnsBackend()) return;
    const std::wstring path = standalone_ui::SettingsPath("RTX40MFG-UI.ini");
    const Setting setting = ReadSetting(path.c_str());
    if (setting == Setting::eOff) return;
    wchar_t line[512]{};
    if (setting == Setting::eInvalid)
    {
        swprintf_s(line, L"GPU name override: off; [%s] %s in %s is not 1 or 0", kSection, kKey, path.c_str());
        single_module::Log(line);
        return;
    }
    gEnabled.store(true, std::memory_order_release);
    const wchar_t* reason = L"";
    bool armed = false;
    {
        std::lock_guard lock(gArming);
        armed = ArmLocked(reason);
    }
    const uint32_t methods = gMethods.load(std::memory_order_acquire);
    wchar_t names[64]{};
    for (size_t i = 0; i < kMethods.size(); ++i)
    {
        if (!(methods & (1u << i))) continue;
        if (names[0]) wcscat_s(names, L",");
        wcscat_s(names, kMethods[i].name);
    }
    swprintf_s(line, L"GPU name override: enabled=1 armed=%d methods=%s%s%s; the game executable sees '%s' (0x%04X) "
        L"for an NVIDIA RTX 20/30 adapter, every other caller the real GPU; processAgeMs=%llu",
        armed ? 1 : 0, names[0] ? names : L"none", armed ? L"" : L" failure=", armed ? L"" : reason,
        kReportedName, kReportedDeviceId, ProcessAgeMs());
    single_module::Log(line);
}

#if MFG_GPU_NAME_OVERRIDE_TESTING
namespace testing
{
void SetEligibleToken(const wchar_t* token) noexcept { gToken.store(token, std::memory_order_release); }
}
#endif
}
