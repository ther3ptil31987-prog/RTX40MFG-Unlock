#pragma once

// Automatic HUDless detection. Observes Streamline resource tags, and on D3D12
// periodically copies a small set of tiles from the game's tagged HUDless
// buffer (on the tagging command list) and from final colour (at Present) into
// owned readback buffers. The worker thread compares them to classify whether
// the game's HUDless image matches final colour outside the UI.
//
// Detection never changes a game option, tag or presented pixel.

#include <cstdint>
#include <string>

struct ID3D12Device;
struct ID3D12CommandQueue;
struct ID3D12Resource;

namespace hudless_probe
{
// Streamline tag description copied while the caller's descriptors are valid.
struct Tag
{
    uint32_t type = 0;
    uint32_t lifecycle = 0;
    void* native = nullptr;
    uint32_t state = 0;
    uint32_t nativeFormat = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t extentLeft = 0;
    uint32_t extentTop = 0;
    uint32_t extentWidth = 0;
    uint32_t extentHeight = 0;
    bool described = false; // ResourceTag/Resource struct identity verified
    // A resource is attached (non-null native), known even when the struct
    // identity cannot be verified. Presence only; never used for COM access.
    bool present = false;
};

using LogSink = void (*)(const wchar_t* message);
void SetLogSink(LogSink sink) noexcept;

// Returns the native IUnknown identity of a D3D12 device after unwrapping
// Streamline and ReShade proxies, or 0. Supplied by the overlay, which owns the
// unwrapping contracts; without it raw COM identities are compared.
using DeviceIdentityResolver = uintptr_t (*)(ID3D12Device* device);
void SetDeviceIdentityResolver(DeviceIdentityResolver resolver) noexcept;

// Graphics API established by slSetD3DDevice/slSetVulkanInfo. Native handles
// are passed to COM only after a D3D12 device has been observed.
void ObserveD3D12Device(void* device) noexcept;
void ObserveVulkan() noexcept;

// Called after an accepted slSetTag/slSetTagForFrame. May record a bounded
// tile copy into the caller's open D3D12 command list while a probe is armed.
// frame is the Streamline frame index of slSetTagForFrame, or kNoFrame.
constexpr uint32_t kNoFrame = UINT32_MAX;
void ObserveTags(const Tag* tags, uint32_t count, void* commandList, bool framed,
    uint32_t frame = kNoFrame) noexcept;

// Frame index of the game's PCL/Reflex ePresentStart marker, sent just before
// Present. With frame-indexed tags it pairs a HUDless image with the Present
// that shows it; engines such as Unreal tag frames ahead of their Present.
void ObservePresentFrame(uint32_t frame) noexcept;
// The frame being presented, when present markers are current.
bool CurrentPresentFrame(uint32_t& frame) noexcept;

// Present side. The overlay asks first, then supplies the current application
// backbuffer (in PRESENT state) with its native device and presentation queue.
bool WantsPresentCapture() noexcept;
void CapturePresent(ID3D12Device* device, ID3D12CommandQueue* queue,
    ID3D12Resource* backbuffer, uint32_t colorSpace) noexcept;

// Worker thread: arms probes, evaluates completed readbacks, logs results.
void Poll() noexcept;

// Status JSON fragment beginning with a comma, for insertion before '}'.
std::string StatusFragment();

// Compares two D3D12 devices after proxy unwrapping (see the resolver above).
bool SameNativeDevice(ID3D12Device* a, ID3D12Device* b) noexcept;

// HUD layer verification, kept for FrameWarp: the game's HUDless is equivalent
// to final colour, either verified in this session or recorded by an earlier
// one ([HUD] RecompositionVerified and RecompositionUi in RTX40MFG-UI.ini), with
// the HUD layer taken from the game's UI buffer or derivable from final colour
// minus HUDless. RTXMFG never enables DLSS-G UI recomposition itself; that is
// left to the game.

// hudless_detection::Hypothesis relating HUDless to final colour in the latest
// equivalent probe (0, identity, until one is measured).
uint32_t HudlessRelation() noexcept;

enum class RecompositionState : uint32_t
{
    eUnknown = 0,
    eGameManaged, // The game enables DLSS-G UI recomposition itself
    eOff,         // The game leaves it off
};
// Reported from the options path; RTXMFG never changes the option.
void ReportRecomposition(RecompositionState state) noexcept;

#if defined(HUDLESS_PROBE_TESTING)
// Test builds only: forget all observations. Readback resources stay retained.
void ResetForTesting() noexcept;
// Test builds only: how long a tagging pattern must persist to count.
void SetSourceSettleForTesting(uint64_t milliseconds) noexcept;
#endif
}
