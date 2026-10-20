#pragma once

#include <Windows.h>

namespace reshade_frontend
{
inline bool ProcessAttach(HMODULE) noexcept { return true; }
inline void ProcessDetach(HMODULE) noexcept {}
}
