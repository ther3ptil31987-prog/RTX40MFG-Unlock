#pragma once
#include <Windows.h>

namespace standalone_ui
{
void Initialize(HMODULE self);
void Draw();
// Right-aligned status for the panel footer (Live / Paused); call after the footer controls.
void DrawFooterStatus();
}
