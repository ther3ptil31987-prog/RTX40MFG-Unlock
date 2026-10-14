#pragma once
#include <Windows.h>
#include <array>
#include <cstdint>
#include <memory>
#include <string>

namespace xess_mod
{
struct Snapshot
{
    bool importsInstalled = false;
    bool providerLoaded = false;
    bool portReady = false;
    bool initialized = false;
    bool enabled = false;
    bool gameEnabled = false;
    bool composition = false;
    bool compositionManagedByGame = false;
    bool compositionRequested = false;
    bool presentStatusAvailable = false;
    bool providerEnabled = false;
    uint32_t applied = 0;
    uint32_t capacity = 0;
    uint32_t framesPresented = 0;
    uint32_t taggedResources = 0;
    uint32_t taggedFrameId = 0;
    uint32_t lastPresentId = 0;
    int32_t lastResult = 0;
    int32_t lastFrameGenResult = 0;
    std::array<uint64_t, 5> resourceAttempts{};
    std::array<uint64_t, 5> resourceAccepted{};
    std::array<int32_t, 5> resourceResults{};
    uint64_t generation = 0;
    uint64_t appFrames = 0;
    uint64_t resourceCalls = 0;
    uint64_t constantsCalls = 0;
    uint64_t lastStateTick = 0;
    // Untracked XeSS-FG contexts: Intel GPUs, an unprofiled pair, or OptiScaler's own.
    // Kept so the menu can tell the game is using XeSS-FG even without MFG.
    bool nativeEnabled = false;
    uint64_t nativeActivityTick = 0;
    uint32_t runtimeSource = 0; // xess_runtime::Source
    uint32_t adapterVendor = 0; // PCI vendor of the game device at XeSS-FG creation
    uint32_t modelActive = 0; // 1 our port of Intel's MFG model, 2 XeFG's 2x model, 0 none yet
    std::string runtimeFailure;
    std::string status = "Waiting for XeSS Frame Generation under OptiScaler.";
    std::string provider;
    std::string adapter;
};

bool InstallImports(HMODULE self) noexcept;
void DiscoverSdkModules() noexcept;
bool PrivateApiHandle(bool fg, void* handle) noexcept;
void InitializeOutsideLoaderLock() noexcept;
std::shared_ptr<const Snapshot> ReadSnapshot() noexcept;
// beta.8: XeSS MFG runs under OptiScaler only; OptiScaler (its menu, or the game through its
// pass-through) sets the frame count. The control file's xessModel (0 our port, 1 XeFG's 2x model)
// and OptiScaler.ini's [XeFG] XeFGModel or earlier RtxmfgModel (0 auto = the control file's, 1 port, 2 XeFG's 2x model)
// choose the model, once, at the first XeFG creation, so a change applies after a restart.
void ConfigureModel(uint32_t model, uint32_t hostModel = 0) noexcept;
void PreparePresent(void* swapchain, uint64_t generation) noexcept;
// XeFG input fixes for tracked contexts (on by default, read once):
//  - matrix fix (dev.38/42): when the game passes XeFG's view and projection in each other's slot
//    (the view slot holds a projection, the projection slot a view; Witcher does), swap them back.
//    RTXMFG_XESS_MATRIX_FIX=0 turns it off.
//  - motion vectors (dev.43, xess/motion_fill.h): 0 the game's own motion vectors, 1 its (2,2)
//    marker filled with camera motion (default; RTXMFG_XESS_MV_FILL=0 selects 0, =2 selects 2),
//    2 all-zero motion (diagnostic).
//  - UI composition (dev.44/47): as the game asks (default); RTXMFG_XESS_UI_COMPOSITION=off keeps it off.
//  - HUD-less match (dev.47, xess/hudless_match.h): 1 the game's HUD-less input converted to the back
//    buffer's encoding before XeFG sees it (default; composition then only for frames it converted),
//    0 the game's own HUD-less input (RTXMFG_XESS_HUDLESS=game).
bool MatrixFixDefault() noexcept;
uint32_t MotionFillDefault() noexcept;
bool UiCompositionOffDefault() noexcept;
uint32_t HudlessMatchDefault() noexcept;
// dev.40 OptiScaler XeFG output: the SDK gateway marks calls made by OptiScaler's own code.
// Contexts such a call creates stay untracked (none of our XeFG changes apply to them); the
// first one applies the frame-count unlock (host_unlock.h) to the profiled provider pair.
// The gateway passes later host calls on those handles straight to the provider.
struct HostCallScope
{
    explicit HostCallScope(bool host) noexcept;
    ~HostCallScope();
    HostCallScope(const HostCallScope&) = delete;
    bool previous;
};
bool HostOwnedHandle(const void* handle, bool fg) noexcept;
void WriteLog(const char* stage, const std::string& fields) noexcept;
HMODULE Self() noexcept;
bool Active() noexcept;
// The game's own discovered SDK pair, once the hooks are active.
HMODULE NativeFgModule() noexcept;
HMODULE NativeLlModule() noexcept;
// beta.8 923: OptiScaler's XeLL input (upstream master 9171bf45 and later) owns the game's XeLL contexts:
// the discovered libxell's create and destroy entries jump into OptiScaler, so none reaches our XeLL hooks.
bool HostOwnsLatency() noexcept;
// Named SDK export authentication bypasses our public Create resolver only.
// The original system resolver/trampoline is published before enablement and
// remains retained for the process lifetime. Callers still verify module owner.
FARPROC RawSdkExport(HMODULE module, LPCSTR name) noexcept;
// A pinned renderer-import wrapper may preserve a selected SDK's real caller
// across its own non-tail GetProcAddress forwarding. Only the two Create names
// qualify; a claimed importer never substitutes for executable caller ownership.
bool ResolveNativeSdkCreate(HMODULE importer, const void* caller, HMODULE module,
    LPCSTR name, FARPROC& result) noexcept;
}
