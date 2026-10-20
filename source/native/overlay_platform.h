#pragma once
#include "single_overlay.h"
#include <imgui.h>
#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <vector>
#include "overlay_slots.h"

namespace single_overlay
{
inline std::recursive_mutex gUiMutex;
inline std::atomic<bool> gVisible{false};
// Pinned panel: stays drawn with the menu closed, translucent and click-through,
// until the menu key reopens it. (The pinned FPS readout is standalone_ui::PinnedFps.)
inline std::atomic<bool> gPinned{false};
inline std::atomic<uint32_t> gRenderFailures{0};
inline std::atomic<uint64_t> gDxgiFrames{0};
inline std::atomic<uint64_t> gVulkanFrames{0};
inline std::atomic<uint64_t> gRenderedFrames{0};
inline std::atomic<bool> gDxgiHooked{false};
inline std::atomic<bool> gVulkanHooked{false};
inline thread_local bool gInsideOverlay = false;

struct InternalScope
{
    bool previous = gInsideOverlay;
    InternalScope() noexcept { gInsideOverlay = true; }
    ~InternalScope() { gInsideOverlay = previous; }
};

struct ContextScope
{
    ImGuiContext* previous;
    explicit ContextScope(ImGuiContext* context) : previous(ImGui::GetCurrentContext())
    { ImGui::SetCurrentContext(context); }
    ~ContextScope() { ImGui::SetCurrentContext(previous); }
};

struct PlatformState
{
    ImGuiContext* context = nullptr;
    HWND window = nullptr;
    std::string iniPath;
    uint64_t firstFrame = 0;
    ImGuiStyle baseStyle;
    float uiScale = 1.0f;
    float layoutScale = 1.0f;
    bool layoutPrepared = false;
    bool inputWasVisible = false;
    bool inputAttached = false;
    // Latin UI font, and per menu catalog the font that draws it (built on first
    // display; state 0 = not built, 1 = covers every glyph, 2 = unavailable).
    ImFont* baseFont = nullptr;
    std::vector<ImFont*> catalogFonts;
    std::vector<uint8_t> catalogFontStates;
    bool Initialize(HWND hwnd);
    void Shutdown();
    void RetireInput();
    bool WantsFrame() const noexcept;
    bool NewFrame(uint32_t outputWidth, uint32_t outputHeight);
    void Draw();
    ImFont* CatalogFont(size_t index);
    void PrepareLanguage();
};

void RecordFailure(const wchar_t* reason) noexcept;

// Bounded-installation support for the input exports (menu-enabled builds
// only): prepare without enabling, then publish after the verified batch.
bool PrepareBoundedInput(HMODULE pinnedUser32, slots::Batch& batch) noexcept;
FARPROC ResolveBoundedInput(HMODULE module, LPCSTR name, FARPROC original) noexcept;
void OnBoundedInputActivated() noexcept;
}
