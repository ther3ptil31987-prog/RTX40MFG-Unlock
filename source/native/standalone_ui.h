#pragma once
#include <Windows.h>

namespace standalone_ui
{
void Initialize(HMODULE self);
void Draw();
// Right-aligned status for the panel footer (Live / Paused); call after the footer controls.
void DrawFooterStatus();
// Pinned FPS readout: whether it is on (the toggle beside the menu's FPS row), and
// its real/DLSS frame rate text for the top-left corner while the menu is closed.
bool PinnedFps();
void DrawPinnedFps();
}
