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
// - UiDelegated(): OptiScaler's menu owns the frame-generation controls.
//   RTXMFG follows the caller's frame-generation requests and applies no
//   latency, V-Sync, frame-limit or DLSS-G preset overrides.
// - UiHidden(): RTXMFG shows no menu at all and applies no DLSS SR/RR preset
//   or DLSS 5 either.
// Control key "optiScalerUi": "shared" = delegated, RTXMFG's menu offers the
// DLSS presets and DLSS 5 and shows only while DLSS SR, DLAA, RR or DLSS 5 runs;
// "optiscaler" = hidden (V1.6.2's behaviour: no factory, swapchain or input
// wrapping and no executable import writes); "rtxmfg" = RTXMFG's own UI and
// controls; absent = DefaultUiMode() (V1.7.0 alpha.19 optimenu.1: shared again,
// hidden for a ForzaTech executable).
namespace optiscaler_host
{
enum class UiMode : uint32_t
{
    Shared = 0,  // RTXMFG's menu: DLSS presets and DLSS 5; frame generation is OptiScaler's.
    Full = 1,    // "rtxmfg": RTXMFG's own UI and controls.
    Hidden = 2,  // "optiscaler": no RTXMFG menu; OptiScaler's menu owns everything.
    Default = 3  // "optiScalerUi" absent: DefaultUiMode().
};

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
    bool uiHidden{};
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
// Control file: "optiScalerUi" ("shared", "rtxmfg", "optiscaler"; Default when absent).
void SetUiMode(UiMode mode) noexcept;
// optimenu.1 (V1.7.0 alpha.19): the mode without "optiScalerUi". Shared (beta.4's default), except Hidden (alpha.16's
// default) for a ForzaTech executable: Forza Motorsport ended after the swapchain while the shared menu wrapped its
// DXGI factory, swapchain and input at startup (B4-01 Forza Horizon 6 the same path); "shared" still opts in there.
// Decided once; loader-lock safe (the version resource and process parameters are read in place).
UiMode DefaultUiMode() noexcept;
// The executable's version resource ProductName or FileDescription names Forza Motorsport or Forza Horizon, or its
// file name starts with "forza" (forza_steamworks_release_final.exe, forzahorizon6.exe).
bool ForzaTechExecutable() noexcept;

bool Detected() noexcept;
bool UiDelegated() noexcept;
bool UiHidden() noexcept;
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
// #490 scene.1: the same key decides XeFG's scene-change detector. True (detector off: its decision is redirected,
// host_unlock.h SceneDetour) when absent, empty, auto or off (also false, no, disabled), and without OptiScaler;
// false for intel (also on, stock), a 0..1 threshold or anything else (Intel's detector). setting receives the raw
// value. Off or intel set no threshold (ReadXefgSceneChangeThreshold returns false with an empty setting).
bool XefgSceneDetectorOff(std::wstring& setting) noexcept;
// V1.5.1 tensorcore.1: OptiScaler.ini [XeFG] XeFGTensorCores beside the identified module. False only for
// off (also 0, false, no, disabled); absent, empty, auto, on and anything else return true (the default:
// the ported model's int8 layers run on tensor cores where the GPU supports it). setting receives the raw value.
bool ReadXefgTensorCores(std::wstring& setting) noexcept;
// flipmeter.1 (test build): OptiScaler.ini [XeFG] XeFGFlipMetering beside the identified module. False only for
// off (also 0, false, no, disabled); absent, empty, auto, on and anything else return true (this test build's
// default: NVIDIA driver flip metering for OptiScaler's XeFG). setting receives the raw value.
bool ReadXefgFlipMetering(std::wstring& setting) noexcept;
// flipmeter.1: OptiScaler.ini [XeFG] XeFGFlipMeteringLeadUs, how early an output inside a metered batch is released,
// in microseconds (0..8000). Returns true with the value when one is set; absent, empty or auto return false with an
// empty setting, anything else returns false with the rejected text in setting.
bool ReadXefgFlipMeteringLead(uint32_t& microseconds, std::wstring& setting) noexcept;
// V1.5.1: OptiScaler.ini [XeFG] XeFGFlipMeteringHotkey beside the identified module, the opt-in for Page Up as the
// live flip-metering toggle (Page Up is also OptiScaler's FPS overlay key and many games bind it). True only for on
// (also 1, true, yes, enabled); absent, empty, off and anything else return false. setting receives the raw value.
bool ReadXefgFlipMeteringHotkey(std::wstring& setting) noexcept;
// V1.6.2-alpha.13 amdflip.1: OptiScaler.ini [XeFG] XeFGAmdFlipMeter beside the identified module (sdk_pacing.h
// SetAmdFlipMeter): 0 for off (also 0, false, no, disabled), 2 for on (also 1, true, yes, enabled: also in a window),
// 1 (auto: exclusive or borderless fullscreen, FSR's rule) for absent, empty, auto and anything else. setting receives
// the raw value.
int ReadXefgAmdFlipMeter(std::wstring& setting) noexcept;
// V1.6.1 pacing.1: OptiScaler.ini [XeFG] XeFGOutputRing beside the identified module, the depth of XeFG's
// generated-output ring (host_unlock.h OutputRing). 1 for 1, off (also 0, false, no, disabled) or stock: XeFG's own
// single entry; absent, empty, auto, on, 2 and anything else return 2 (the default). setting receives the raw value.
uint8_t ReadXefgOutputRing(std::wstring& setting) noexcept;
// V1.6.2-alpha.20 hdrpq.1: OptiScaler.ini [XeFG] XeFGHdr10Flag beside the identified module. False (XeFG's own HDR10
// handling, host_unlock.h Hdr10Flag not applied) only for stock (also intel, on, 1, true, yes, enabled); absent, empty,
// auto, off and anything else return true (the default: the flag stays off), as does no OptiScaler. setting receives
// the raw value.
bool ReadXefgHdr10FlagOff(std::wstring& setting) noexcept;
// V1.6.1 pacing.1: OptiScaler.ini [XeFG] XeFGFlipQueue beside the identified module (sdk_pacing.h SetFlipQueue). False
// only for off (also 0, false, no, disabled): V1.6.0's CPU pacing; absent, empty, auto, on and anything else return true.
bool ReadXefgFlipQueue(std::wstring& setting) noexcept;
// vkxefg.2: the ini sets XeFGFlipQueue itself (present, not empty, not auto).
bool XefgFlipQueueSet() noexcept;
// V1.7.0 firstready.1: OptiScaler.ini [XeFG] XeFGFlipQueueFirst beside the identified module (sdk_pacing.h
// SetFlipQueueFirst). False only for buffer (also buffered, off, 0, false, no, disabled): the jitter buffer before each
// batch's first output as in V1.6.1-V1.7.0 alpha.13; absent, empty, auto, ready and anything else return true (first
// outputs at readiness). setting receives the raw value.
bool ReadXefgFlipQueueFirst(std::wstring& setting) noexcept;
// V1.6.1 pacing.1: OptiScaler.ini [XeFG] XeFGFlipQueueBufferPct, the flip queue's base jitter buffer in percent of a batch
// (0..100). True with the value when one is set; absent, empty or auto return false with an empty setting, anything
// else returns false with the rejected text in setting.
bool ReadXefgFlipQueueBuffer(uint32_t& percent, std::wstring& setting) noexcept;
// V1.6.2-alpha.5 menucap.1: the most back buffers XeFG's native swap chain may get under OptiScaler. OptiScaler's DX12
// overlay menu (menu/menu_overlay_dx.cpp, NUM_BACK_BUFFERS = 8; upstream master unchanged) draws on XeFG's native chain
// and keeps one command allocator, back buffer and RTV handle per buffer in arrays of 8, but CreateRenderTargetDx12 fills
// them for every buffer the chain has. pacing.1's default of 10 wrote buffers 8 and 9 over the globals after the arrays
// (the menu's blur and present-queue pointers; RTV handles 0 and 1). CoD MW4 (RTX 40, OptiScaler 10.0.0-dev as
// WINMM.dll, beta.1): "native_buffers requested 3 created 10", then no frame-generation call ever followed.
constexpr uint32_t kMenuBackBuffers = 8;
// V1.6.2-alpha.9 menu16.1: upstream fixed the menu (optiscaler/OptiScaler PR 1229, master d06f31e9, 21 Oct 2026): its
// arrays and the blur's descriptor slots hold 16 buffers (DXGI's maximum), CreateRenderTargetDx12 refuses larger chains
// and logs "Swapchain has {} buffers, menu supports {}". That text is the marker: MenuBackBuffers() is 16 only when the
// identified OptiScaler image carries it in an initialized read-only data section, and kMenuBackBuffers otherwise
// (older OptiScaler builds, forks without the fix, or no OptiScaler identified yet). Decided once per image.
constexpr uint32_t kFixedMenuBackBuffers = 16;
constexpr char kMenuFixMarker[] = "Swapchain has {} buffers, menu supports {}";
uint32_t MenuBackBuffers() noexcept;
// V1.6.1 pacing.1: OptiScaler.ini [XeFG] XeFGNativeBuffers, the back buffers of XeFG's native swap chain while the flip
// queue is on (sdk_pacing.h NativeBuffersScope). Absent, empty or auto: 10 (pacing.1's) under an OptiScaler with the menu
// fix, 8 otherwise (menucap.1); off (also 0, false, no, disabled): the game's own count (0 is returned); 2..16: that many,
// capped at MenuBackBuffers(). Anything else returns the default with the rejected text in setting. requested receives
// the count before the cap (0 for off).
constexpr uint32_t kDefaultNativeBuffers = 10;
uint32_t ReadXefgNativeBuffers(std::wstring& setting, uint32_t& requested) noexcept;

// fakenvapi (bundled with OptiScaler; built into OptiScaler v10) answers
// nvapi_QueryInterface for its private interface 0x21382138, and a separate
// copy is a game-folder nvapi64.dll. Either is not NVIDIA's driver API, so the
// NVAPI-dependent parts fail closed. Not for use under the loader lock.
using NvapiQueryInterface = void* (__cdecl*)(uint32_t);
bool IsFakeNvapi(HMODULE nvapi, NvapiQueryInterface query) noexcept;
void NoteFakeNvapi() noexcept;
bool FakeNvapiObserved() noexcept;
}
