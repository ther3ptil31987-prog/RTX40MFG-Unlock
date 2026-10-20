// RTX 30 detection under a layer that detours NVAPI's factory, as OptiScaler
// does: an inline hook on System32 nvapi64.dll's nvapi_QueryInterface that
// returns its own GetArchInfo wrapper (OptiScaler NvApiHooks.cpp). Needs the
// installed NVIDIA driver with the split driver-store nvapi64_impl.dll. All
// hooks stay inside this test process; no NVIDIA file is changed.
#include "adapter_discovery.h"
#include "ampere_provider_arch.h"
#include <MinHook.h>
#include <iostream>
#include <string>
unsigned checks = 0, failures = 0;
void Check(bool ok, const char* text)
{ ++checks; failures += !ok; std::cout << (ok ? "PASS " : "FAIL ") << text << '\n'; }

namespace
{
using adapter_discovery::QueryInterface;
constexpr uint32_t kInitialize = 0x0150e828, kEnumerate = 0xe5ac921f, kArchitecture = 0xd8265d24;
constexpr std::array<uint32_t, 3> kDetectionIds{kInitialize, kEnumerate, kArchitecture};
constexpr std::array<uint32_t, 5> kAdapterIds{kInitialize, kEnumerate, kArchitecture, 0xadd604d1, 0x842b066e};

enum class Replacement { eNone, eArchitecture, eInitialize, eEnumerate, eArchitectureNull, eArchitecturePrivate };
Replacement gReplacement = Replacement::eNone;
QueryInterface gOriginalDispatcher = nullptr, gOriginalImplementation = nullptr;
bool gImplementationDetoured = false;
unsigned gForeignCalls = 0;
void* gPrivateCode = nullptr;

// Reports an RTX 3090 (GA102) so that calling it would arm RTX 30 on any GPU.
int __cdecl ForeignArchitecture(void*, adapter_discovery::Architecture* info)
{
    ++gForeignCalls;
    if (info) { info->architecture = ampere_provider_arch::kAmpereArchitecture; info->implementation = 2; }
    return 0;
}
int __cdecl ForeignInitialize() { ++gForeignCalls; return 0; }

void* __cdecl DetouredDispatcher(uint32_t id)
{
    switch (gReplacement)
    {
    case Replacement::eArchitecture: if (id == kArchitecture) return reinterpret_cast<void*>(&ForeignArchitecture); break;
    case Replacement::eInitialize: if (id == kInitialize) return reinterpret_cast<void*>(&ForeignInitialize); break;
    case Replacement::eEnumerate: if (id == kEnumerate) return reinterpret_cast<void*>(&ForeignInitialize); break;
    case Replacement::eArchitectureNull: if (id == kArchitecture) return nullptr; break;
    case Replacement::eArchitecturePrivate: if (id == kArchitecture) return gPrivateCode; break;
    default: break;
    }
    return gOriginalDispatcher(id);
}
void* __cdecl DetouredImplementation(uint32_t id)
{
    if (gImplementationDetoured && id == kArchitecture) return reinterpret_cast<void*>(&ForeignArchitecture);
    return gOriginalImplementation(id);
}

std::wstring ModulePath(HMODULE module)
{
    wchar_t path[1024]{};
    const DWORD length = GetModuleFileNameW(module, path, 1024);
    return length && length < 1024 ? std::wstring(path, length) : std::wstring();
}
bool EndsWith(const std::wstring& text, const wchar_t* suffix)
{
    const size_t length = wcslen(suffix);
    return text.size() >= length && _wcsicmp(text.c_str() + text.size() - length, suffix) == 0;
}
}

int wmain()
{
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    HMODULE dispatcher = LoadLibraryExW(L"nvapi64.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!dispatcher) { std::cout << "SETUP no System32 nvapi64.dll (an NVIDIA driver is required)\n"; return 2; }

    adapter_discovery::NvapiTargets<kAdapterIds.size()> inventory;
    const bool inventoried = adapter_discovery::ResolveNvapi(kAdapterIds, inventory);
    for (size_t i = 0; inventoried && i < kAdapterIds.size(); ++i)
        std::wcout << L"INFO id 0x" << std::hex << kAdapterIds[i] << std::dec << L" owner "
            << ModulePath(inventory.owners[i].module) << L'\n';
    adapter_discovery::NvapiTargets<kDetectionIds.size()> base;
    Check(adapter_discovery::ResolveNvapi(kDetectionIds, base) && base.redirected == 0,
        "unhooked NVAPI resolves every detection id through the dispatcher");
    // 617.42: the dispatcher keeps Initialize; Enumerate and GetArchInfo are nvapi64_impl.dll's.
    const HMODULE implementation = base.owners[1].module;
    const std::wstring implementationPath = ModulePath(implementation);
    std::wcout << L"INFO implementation " << implementationPath << L'\n';
    if (implementation == dispatcher || !EndsWith(implementationPath, L"\\nvapi64_impl.dll")
        || base.owners[2].module != implementation)
    {
        std::cout << "SETUP this driver has no split nvapi64_impl.dll factory for Enumerate and GetArchInfo\n";
        return 2;
    }
    void* const realArchitecture = base.targets[2];

    const auto dispatcherQuery = reinterpret_cast<void*>(GetProcAddress(dispatcher, "nvapi_QueryInterface"));
    const auto implementationQuery = reinterpret_cast<void*>(GetProcAddress(implementation, "nvapi_QueryInterface"));
    Check(MH_Initialize() == MH_OK
        && MH_CreateHook(dispatcherQuery, reinterpret_cast<void*>(&DetouredDispatcher),
            reinterpret_cast<void**>(&gOriginalDispatcher)) == MH_OK
        && MH_CreateHook(implementationQuery, reinterpret_cast<void*>(&DetouredImplementation),
            reinterpret_cast<void**>(&gOriginalImplementation)) == MH_OK
        && MH_EnableHook(MH_ALL_HOOKS) == MH_OK, "fixture detours both NVAPI factories inline in this process");

    gReplacement = Replacement::eArchitecture;
    adapter_discovery::NvapiTargets<kDetectionIds.size()> hooked;
    Check(reinterpret_cast<void*>(QueryInterface(dispatcherQuery)(kArchitecture)) == reinterpret_cast<void*>(&ForeignArchitecture),
        "the detoured dispatcher returns the foreign GetArchInfo wrapper");
    Check(adapter_discovery::ResolveNvapi(kDetectionIds, hooked) && hooked.redirected == 0x4
        && hooked.targets[2] == realArchitecture && hooked.owners[2].module == implementation,
        "a foreign GetArchInfo is replaced by the driver implementation's own function");
    adapter_discovery::NvapiTargets<kAdapterIds.size()> adapter;
    Check(adapter_discovery::ResolveNvapi(kAdapterIds, adapter) && adapter.redirected == 0x4,
        "adapter discovery resolves its five ids under the same detour");

    // The full detection under the detour must report this machine's real GPU,
    // never the foreign function's RTX 3090.
    const bool armed = ampere_provider_arch::DetectAndArm();
    const auto snapshot = ampere_provider_arch::Read();
    adapter_discovery::Architecture real{sizeof(adapter_discovery::Architecture) | (2u << 16), 0, 0, 0};
    std::array<void*, 64> physical{};
    uint32_t count = 0;
    const bool readReal = reinterpret_cast<adapter_discovery::Initialize>(base.targets[0])() == 0
        && reinterpret_cast<adapter_discovery::Enumerate>(base.targets[1])(physical.data(), &count) == 0 && count
        && reinterpret_cast<adapter_discovery::GetArchitecture>(realArchitecture)(physical[0], &real) == 0;
    std::cout << "INFO gpus=" << count << " architecture=0x" << std::hex << real.architecture
        << " implementation=0x" << real.implementation << std::dec << '\n';
    Check(readReal && snapshot.detected && snapshot.nvapiRedirected == 0x4 && snapshot.physicalGpuCount == count
        && (count != 1 || (snapshot.singleGpuArchitecture == real.architecture
            && snapshot.singleGpuImplementation == real.implementation)),
        "detection under the detour reads the GPU through NVIDIA's function and records the redirect");
    Check(armed == ampere_provider_arch::IsAmpereRtx(real.architecture, real.implementation) && gForeignCalls == 0,
        "detection arms only for a real RTX 30 GPU and never calls the foreign wrapper");

    gReplacement = Replacement::eInitialize;
    adapter_discovery::NvapiTargets<kDetectionIds.size()> first;
    Check(!adapter_discovery::ResolveNvapi(kDetectionIds, first),
        "a foreign first id has no established implementation to ask and resolves nothing");
    gReplacement = Replacement::eEnumerate;
    adapter_discovery::NvapiTargets<kDetectionIds.size()> beforeImplementation;
    Check(!adapter_discovery::ResolveNvapi(kDetectionIds, beforeImplementation),
        "a foreign id before any implementation-owned id resolves nothing (the dispatcher-owned id is no implementation)");
    gReplacement = Replacement::eArchitectureNull;
    adapter_discovery::NvapiTargets<kDetectionIds.size()> missing;
    Check(!adapter_discovery::ResolveNvapi(kDetectionIds, missing), "a missing function is not replaced");
    gPrivateCode = VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READ);
    gReplacement = Replacement::eArchitecturePrivate;
    adapter_discovery::NvapiTargets<kDetectionIds.size()> privateCode;
    Check(gPrivateCode && adapter_discovery::ResolveNvapi(kDetectionIds, privateCode) && privateCode.redirected == 0x4
        && privateCode.targets[2] == realArchitecture, "unowned private code is replaced by NVIDIA's function too");
    gReplacement = Replacement::eArchitecture;
    gImplementationDetoured = true;
    adapter_discovery::NvapiTargets<kDetectionIds.size()> both;
    Check(!adapter_discovery::ResolveNvapi(kDetectionIds, both),
        "a detoured implementation factory returning foreign code is not trusted either");
    gImplementationDetoured = false;
    Check(gForeignCalls == 0, "no resolution called a foreign function");

    gReplacement = Replacement::eNone;
    Check(MH_DisableHook(MH_ALL_HOOKS) == MH_OK && MH_Uninitialize() == MH_OK, "fixture removes both detours");
    adapter_discovery::NvapiTargets<kDetectionIds.size()> restored;
    Check(adapter_discovery::ResolveNvapi(kDetectionIds, restored) && restored.redirected == 0
        && restored.targets == base.targets, "restored NVAPI resolves exactly as before");
    if (gPrivateCode) VirtualFree(gPrivateCode, 0, MEM_RELEASE);
    std::cout << checks << " checks, " << failures << " failures\n";
    return failures ? 1 : 0;
}
