// OptiScaler stand-in for the release harness. Its version resource carries
// OptiScaler's identity strings (ProductName "OptiScaler", OriginalFilename
// "OptiScaler.dll", InternalName "OptiScaler") and it exports the NGX facade
// shape OptiScaler has (D3D12 CreateFeature/EvaluateFeature/
// GetFeatureRequirements/Init_ProjectID) plus a fakenvapi-style
// nvapi_QueryInterface, so RTXMFG's classification of those exports is
// exercised. Like OptiScaler's [Plugins] LoadAsiPlugins, DllMain loads the
// .asi named by RTXMFG_TEST_OPTISCALER_PLUGIN with the loader lock held and
// calls its InitializeASI on the same thread.
//
// Built twice: without OPTISCALER_FIXTURE_NO_IDENTITY as the host, and with it
// as a fakenvapi stand-in (copied to nvapi64.dll) that has no OptiScaler
// identity.
#include <Windows.h>
#include <cstdint>

namespace
{
HMODULE gPlugin = nullptr;
int gInitializeCalled = 0;
int gPluginLoadError = 0;
}

extern "C" __declspec(dllexport) int __cdecl NVSDK_NGX_D3D12_CreateFeature() { return -1; }
extern "C" __declspec(dllexport) int __cdecl NVSDK_NGX_D3D12_EvaluateFeature() { return -1; }
extern "C" __declspec(dllexport) int __cdecl NVSDK_NGX_D3D12_GetFeatureRequirements() { return -1; }
extern "C" __declspec(dllexport) int __cdecl NVSDK_NGX_D3D12_Init_ProjectID() { return -1; }
extern "C" __declspec(dllexport) int __cdecl NVSDK_NGX_D3D12_ReleaseFeature() { return -1; }

extern "C" __declspec(dllexport) void* __cdecl nvapi_QueryInterface(uint32_t id)
{
    static int marker = 0;
    return id == 0x21382138u ? &marker : nullptr;
}

extern "C" __declspec(dllexport) void __cdecl OptiFixtureReport(int* loaded, int* initialized, int* error)
{
    if (loaded) *loaded = gPlugin != nullptr;
    if (initialized) *initialized = gInitializeCalled;
    if (error) *error = gPluginLoadError;
}

BOOL WINAPI DllMain(HINSTANCE, DWORD reason, LPVOID)
{
#if !defined(OPTISCALER_FIXTURE_NO_IDENTITY)
    if (reason == DLL_PROCESS_ATTACH)
    {
        wchar_t plugin[MAX_PATH]{};
        const DWORD length = GetEnvironmentVariableW(L"RTXMFG_TEST_OPTISCALER_PLUGIN", plugin, MAX_PATH);
        if (length && length < MAX_PATH)
        {
            gPlugin = LoadLibraryExW(plugin, nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
            if (!gPlugin) gPluginLoadError = static_cast<int>(GetLastError());
            using InitializeAsi = void(*)();
            if (gPlugin)
                if (auto initialize = reinterpret_cast<InitializeAsi>(GetProcAddress(gPlugin, "InitializeASI")))
                {
                    initialize();
                    gInitializeCalled = 1;
                }
        }
    }
#endif
    return TRUE;
}
