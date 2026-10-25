#pragma once
// xemfgsa.1 (V1.7.0): XeMFG standalone on D3D12. RTXMFG is the XeFG host without OptiScaler: the DXGI factory proxy
// (overlay_dxgi_proxy.cpp) asks here before it creates a game's swap chain, and when the host takes it, XeFG creates
// the real chain (on a system DXGI factory, so frame generators above us see one present per frame) and the game gets
// XeFG's proxy chain, wrapped by the menu's proxy like any other. Every frame's depth and motion vectors are tagged on
// the game's own command lists as they arrive (XeFG copies what is valid only then), from Streamline's DLSS-G tags or
// from the upscaler the game evaluates (xess/standalone_policy.h). The game's own DLSS Frame Generation is held at Off
// while the host replaces it (patcher.cpp HandlePublicSetOptions).
//
// libxess_fg.dll and libxell.dll are never bundled: the player's copy in an RTXMFG-XeSS folder beside MFG Unlocker or
// the game, or beside either, as for the Vulkan host. Everything fails closed: a chain, frame or input that does not
// meet every requirement stays the game's own.
#include <Windows.h>
#include <dxgi1_6.h>
#include <cstdint>
#include <string>

struct ID3D12GraphicsCommandList;
struct NVSDK_NGX_Parameter;
namespace sl { struct ResourceTag; struct Constants; }

namespace xess_d3d12
{
// The menu's XeMFG standalone switch and frame count (2..6), read at start and on every control change. Takeover
// happens only at a swap chain's creation, so switching on applies after a restart (or the game's next chain).
void Configure(bool enabled, uint32_t frames) noexcept;
bool Enabled() noexcept;
// xemfgsa.5: the menu's FPS limit in presented frames per second (0 off), applied as XeLL's minimum interval between
// the game's frames while the game's frames sleep in XeLL.
void SetFpsLimit(uint32_t fps) noexcept;
// xemfgsa.6: Dynamic MFG (xess/xemfg_dynamic.h): the count follows the real frame rate toward the target (0: the FPS
// limit, else just under the refresh rate), up to Configure's frames; rounding 0 at most, 1 nearest, 2 at least; 1X
// (generation off) when allowed; "Use target as cap" holds the target with the XeLL limit. Applies live.
void SetDynamic(bool enabled, uint32_t targetFps, int rounding, bool allow1x, bool targetAsCap) noexcept;

// DXGI factory proxy, CreateSwapChainForHwnd / CreateSwapChain: true when the host created the chain (hr and *chain
// are the result), false to create it natively. `caller` is the creation's return address.
bool CreateSwapChain(IUnknown* queue, HWND window, const DXGI_SWAP_CHAIN_DESC1* desc,
    const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fullscreen, IDXGIOutput* output, void* caller, IDXGISwapChain1** chain,
    HRESULT& hr) noexcept;
// This thread is inside CreateSwapChain's takeover: XeFG's output chain created now takes the menu
// (overlay_dx12.cpp OutputSwapchainWanted, AttachOutputSwapchain), the game's chain gets a menu-less proxy.
bool InsideTakeover() noexcept;
// The menu proxy could not wrap the host's chain (it would never see its presents or its release): the chain
// returned by CreateSwapChain is released and its XeFG context destroyed; the caller then creates it natively.
void Abandon(IDXGISwapChain* chain) noexcept;
bool Owns(IDXGISwapChain* chain) noexcept;
// Around the game's own Present/Present1 on a host chain (the menu proxy's outer call, not DXGI_PRESENT_TEST).
void BeforePresent(IDXGISwapChain* chain) noexcept;
void AfterPresent(IDXGISwapChain* chain, HRESULT result) noexcept;
// The menu proxy released its reference to the chain (the game's last): XeFG's context goes.
void ChainReleased(IDXGISwapChain* chain) noexcept;

// While the host runs (or may still take a chain over), the game's DLSS Frame Generation is held at Off and its
// on/off request switches XeMFG instead.
bool ReplacesFrameGeneration() noexcept;
void GameFrameGeneration(bool on) noexcept;

// Inputs. Streamline: after the game's slSetTag/slSetTagForFrame and slSetConstants returned eOk (patcher.cpp);
// `framed` false for the legacy slSetTag, whose frame is the latest token seen.
void StreamlineTags(uint32_t viewport, bool framed, uint32_t frame, const sl::ResourceTag* tags, uint32_t count,
    void* commandList) noexcept;
void StreamlineConstants(const sl::Constants& constants, uint32_t frame, uint32_t viewport) noexcept;
// Streamline's PCL/Reflex markers (sl::PCLMarker 0..5 = XeLL's markers) and the game's Reflex sleep; `slept` when
// the game's own sleep ran (XeLL then takes the markers only).
void LatencyMarker(uint32_t marker, uint32_t frame) noexcept;
void LatencySleep(uint32_t frame, bool slept) noexcept;
// xemfgsa.2: while XeMFG generates, the game's Reflex sleep is XeLL's (as OptiScaler's XeLL output does with XeFG):
// the driver's Reflex would pace the game against XeFG's extra presents, a second pacer on top of XeLL's.
bool ReplacesReflexSleep() noexcept;
// A DLSS Super Resolution / Ray Reconstruction D3D12 evaluation is about to run on this list (ampere_backend.cpp's
// NGX thunks, before the real Evaluate).
void CaptureDlssEvaluate(ID3D12GraphicsCommandList* list, const NVSDK_NGX_Parameter* parameters) noexcept;
// Hooks the XeSS SR (libxess) and FidelityFX API (amd_fidelityfx_*_dx12) modules the process loaded; the worker
// calls it on module changes and every few seconds.
void DiscoverUpscalers() noexcept;

struct Snapshot
{
    bool enabled = false;
    bool active = false;           // a game chain presents through XeFG
    bool generating = false;       // XeFG generated for the last frames
    bool restartNeeded = false;    // switched on after the game's chain was created natively
    bool gameFrameGeneration = false, dlssgSeen = false, fsrFrameGeneration = false;
    uint32_t frames = 0;           // XeFG's presented frames per base frame (generated + 1) once applied
    uint32_t source = 0;           // xemfg_standalone::Source of the last frame
    uint64_t basePresents = 0, inputFrames = 0;
    // xemfgsa.5: the game's and XeFG's frame rates (FPS x 1000) and the sample's age (UINT64_MAX before the first);
    // the FPS limit and the XeLL interval it set (0 = none).
    uint32_t baseFpsMilli = 0, outputFpsMilli = 0;
    uint64_t fpsAgeMs = UINT64_MAX;
    uint32_t fpsLimit = 0, limitIntervalUs = 0;
    // xemfgsa.6: Dynamic MFG runs (frames is its count, 1 = generation off), its target (FPS x 1000) and its cap mode.
    bool dynamic = false, dynamicCap = false;
    uint32_t dynamicTargetMilli = 0;
    std::string state = "off";
    std::string reason;
    std::string runtime;           // the folder XeFG was loaded from
    // alpha.15 (B4-12), after a failed search: the RTXMFG-XeSS folder beside MFG Unlocker, the game's EXE folder and
    // the first near-miss name (UTF-8).
    std::string runtimeSearch, runtimeGame, runtimeMisnamed;
};
Snapshot ReadSnapshot() noexcept;
}
