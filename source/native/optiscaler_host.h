#pragma once
#include <Windows.h>
#include <cstddef>
#include <cstdint>
#include <string>

// OptiScaler cooperation. OptiScaler (github.com/optiscaler/OptiScaler) is a
// separate mod that can share the process with RTXMFG, either as the host that
// loads MFGUnlocker.asi from its plugins folder (InitializeASI, called inside its
// own DllMain under the loader lock) or as a sibling proxy DLL. Identity comes
// from the mapped image's version resource only; the module name, folder and
// proxy filename are not identity.
//
// Two separate decisions:
// - Detected(): OptiScaler is in the process. Cooperation rules always apply
//   (its NGX facade is never treated as an NVIDIA runtime, the GPU vendor is
//   read below its DXGI spoofing, its XeSS FG/XeLL calls are discovered).
//   beta.8: XeSS MFG runs only while it holds; without OptiScaler a game's own
//   XeSS frame generation stays untouched (xess_mod::DiscoverSdkModules).
// - UiDelegated(): OptiScaler's menu owns the user controls. RTXMFG shows no
//   UI, follows the caller's frame-generation requests and applies no latency,
//   V-Sync, frame-limit or preset overrides. Default whenever detected; the
//   control key "optiScalerUi":"rtxmfg" restores RTXMFG's own UI and controls.
namespace optiscaler_host
{
enum class Route : uint32_t
{
    None = 0,
    Proxy = 1,   // OptiScaler was mapped when RTXMFG started (sibling proxy).
    Plugin = 2,  // OptiScaler called RTXMFG's InitializeASI.
    Late = 3     // OptiScaler was mapped after RTXMFG started.
};

struct Snapshot
{
    Route route{Route::None};
    bool detected{};
    bool uiDelegated{};
    bool uiOptIn{};
    bool pluginCallerAccepted{};
    bool pluginCallerRejected{};
    uintptr_t base{};
    uint32_t imageSize{};
    wchar_t fileVersion[32]{};
    wchar_t productVersion[96]{};
    wchar_t path[MAX_PATH]{};
};

// Loader-lock safe: walks the mapped image's resource directory directly.
bool ReadVersionString(HMODULE module, const wchar_t* key,
    wchar_t* output, size_t count) noexcept;
bool IsOptiScalerImage(HMODULE module) noexcept;

// DllMain, before any hook is armed. Scans the already-mapped modules.
void ObserveProcessStart(HMODULE self) noexcept;
// Loader notification for a newly mapped image (loader lock held).
void ObserveMappedModule(HMODULE module) noexcept;
// InitializeASI. Returns true when the caller is the detected OptiScaler image.
bool AcceptPluginCaller(HMODULE caller) noexcept;
// Control file: "optiScalerUi":"rtxmfg" sets the opt-in.
void SetUiOptIn(bool rtxmfgUi) noexcept;

bool Detected() noexcept;
bool UiDelegated() noexcept;
Route CurrentRoute() noexcept;
HMODULE Module() noexcept;
bool IsHostModule(HMODULE module) noexcept;
bool IsHostAddress(const void* address) noexcept;
Snapshot Read() noexcept;
const char* RouteName(Route route) noexcept;

// The model RTXMFG runs OptiScaler's XeFG on: [XeFG] XeFGModel in the OptiScaler.ini beside
// the identified module (xefgmodel.1; the earlier key RtxmfgModel is read only when XeFGModel
// is absent or empty). 0 auto or absent, 1 port, 2 default (XeFG's own 2x model; the earlier
// value intel too); setting, path and key receive the raw value, the ini path and the key it
// came from (empty without OptiScaler or either key). File I/O: not under the loader lock.
uint32_t ReadXefgModel(std::wstring& setting, std::wstring& path, std::wstring& key) noexcept;
// scenechange.1: OptiScaler.ini [XeFG] XeFGSceneChangeThreshold beside the identified module. Returns true with a
// value in [0, 1] when one is set; absent, empty or auto return false with an empty setting, anything else returns
// false with the rejected text in setting.
bool ReadXefgSceneChangeThreshold(float& threshold, std::wstring& setting) noexcept;

// fakenvapi (bundled with OptiScaler; built into OptiScaler v10) answers
// nvapi_QueryInterface for its private interface 0x21382138, and a separate
// copy is a game-folder nvapi64.dll. Either is not NVIDIA's driver API, so the
// NVAPI-dependent parts fail closed. Not for use under the loader lock.
using NvapiQueryInterface = void* (__cdecl*)(uint32_t);
bool IsFakeNvapi(HMODULE nvapi, NvapiQueryInterface query) noexcept;
void NoteFakeNvapi() noexcept;
bool FakeNvapiObserved() noexcept;
}
