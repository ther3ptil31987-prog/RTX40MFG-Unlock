#pragma once

// Present-time passes over the game's tagged HUDless image. D3D12 only.
//
// Debug tint: paints every presented pixel whose final colour differs from the
// HUDless image, i.e. what the HUDless image leaves out. Session-only, off by
// default; changes presented pixels only while enabled. RTXMFG_HUDLESS_TINT=1
// enables it at start for unattended diagnostics.
//
// UI synthesis: for games that tag HUDless without a UI buffer, derives a UI
// alpha image from final colour and the same frame's HUDless and tags it as
// Streamline's UI alpha buffer for the Present, so DLSS-G UI recomposition can
// run. Each pixel's opacity is learned from how final colour follows HUDless
// changes while the scene moves behind the UI; until then (and for UI that
// keeps changing) it is the smallest alpha explaining the frame, which is exact
// for black or white UI but too low for coloured translucent panels. Never
// changes presented pixels. Runs only while HUDless detection has verified the
// game for it.

#include <cstdint>

struct ID3D12Device;
struct ID3D12CommandQueue;
struct ID3D12Resource;

namespace hudless_visualizer
{
void SetEnabled(bool enabled) noexcept;
bool Enabled() noexcept;

// Tag hook (game thread): the latest HUDless tag. eOnlyValidNow contents are
// copied on the tagging command list; later lifecycles are read at Present.
// frame is the tag's Streamline frame index (hudless_probe::kNoFrame if none);
// with present markers it selects the copy belonging to the presented frame.
void ObserveHudless(void* resource, uint32_t state, uint32_t lifecycle,
    uint32_t extentLeft, uint32_t extentTop, uint32_t extentWidth, uint32_t extentHeight,
    void* commandList, uint32_t frame) noexcept;
// True while the tint or UI synthesis needs HUDless images.
bool WantsHudless() noexcept;

// Present (before the menu draws): tints the application backbuffer, which is
// in PRESENT state, on the presentation queue.
void Draw(ID3D12Device* device, ID3D12CommandQueue* queue, ID3D12Resource* backbuffer) noexcept;

// Short state for the Debug panel, e.g. "drawing", "waiting for HUDless".
const char* StateText() noexcept;
uint64_t FramesDrawn() noexcept;
// Frames tinted from the copy tagged for the presented frame.
uint64_t PairedFrames() noexcept;

// Tags a UI alpha texture (R8_UNORM, in `state`) for the Present of `frame`
// (hudless_probe::kNoFrame for games tagging without frame indices). Supplied by
// the Streamline hook owner. Returns true when Streamline accepted the tag.
using UiTagger = bool (*)(void* resource, uint32_t state, uint32_t width, uint32_t height,
    uint32_t format, uint32_t frame);
void SetUiTagger(UiTagger tagger) noexcept;
bool SynthesisSupported() noexcept;

// Set by HUDless detection: synthesis is verified for this game (desired) or
// recomposition was latched with a synthesized UI buffer (must continue).
void SetSynthesisActive(bool active) noexcept;
bool SynthesisActive() noexcept;

// Present, after the RTXMFG menu drew (so the menu counts as UI): derives and
// tags this frame's UI alpha. When this frame cannot be derived (no HUDless for
// it, partial update, GPU busy), the last alpha is tagged again, and before the
// first one a cleared alpha (no UI), so the UI buffer is never missing while
// recomposition relies on it.
void SynthesizeUi(ID3D12Device* device, ID3D12CommandQueue* queue, ID3D12Resource* backbuffer,
    bool reuseOnly) noexcept;
// A freshly derived alpha was tagged within the last second.
bool SynthesisHealthy() noexcept;
const char* SynthesisStateText() noexcept;
uint64_t SynthesizedFrames() noexcept;
uint64_t SynthesisReusedFrames() noexcept;
// "learned" (per-pixel opacity state in use), "per-frame" (state could not be
// allocated) or "none" before the first derived alpha.
const char* SynthesisAlphaText() noexcept;
// Passes that restarted learning (first use, new size, synthesis restarted).
uint64_t SynthesisPrimes() noexcept;
}
