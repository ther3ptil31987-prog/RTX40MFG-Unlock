#pragma once
#include <Windows.h>
#include <cstdio>
#include <string>

// Key and mouse-button state below other mods' input hooks. OptiScaler's
// input layer, for one, answers GetAsyncKeyState with "up" to every caller
// while its menu is open; win32u's NtUserGetAsyncKeyState, which
// GetAsyncKeyState calls, is not hooked by it.
inline bool PhysicalKeyDown(int key) noexcept
{
    using AsyncKeyState = SHORT (WINAPI*)(int);
    static const AsyncKeyState read = []
    {
        const HMODULE win32u = GetModuleHandleW(L"win32u.dll");
        const auto entry = win32u
            ? reinterpret_cast<AsyncKeyState>(GetProcAddress(win32u, "NtUserGetAsyncKeyState")) : nullptr;
        return entry ? entry : &GetAsyncKeyState;
    }();
    return key > 0 && key < 256 && (read(key) & 0x8000) != 0;
}

// Whether a window of this process is in the foreground.
inline bool ProcessInForeground() noexcept
{
    DWORD process = 0;
    const HWND window = GetForegroundWindow();
    return window && GetWindowThreadProcessId(window, &process) && process == GetCurrentProcessId();
}

// A key's name in the keyboard layout ("Backspace", "=", "F5"), else its code.
inline std::string KeyName(int key)
{
    if (key == VK_BACK) return "Backspace";
    if (key == VK_OEM_PLUS) return "=";
    if (key == VK_XBUTTON1) return "Mouse 4";
    if (key == VK_XBUTTON2) return "Mouse 5";
    const UINT scan = MapVirtualKeyW(key, MAPVK_VK_TO_VSC_EX);
    const LONG parameter = LONG((scan & 0xFF) << 16) | (scan & 0xFF00 ? 1 << 24 : 0);
    wchar_t label[96]{};
    if (GetKeyNameTextW(parameter, label, static_cast<int>(std::size(label))))
    {
        const int bytes = WideCharToMultiByte(CP_UTF8, 0, label, -1, nullptr, 0, nullptr, nullptr);
        std::string text(bytes, '\0');
        WideCharToMultiByte(CP_UTF8, 0, label, -1, text.data(), bytes, nullptr, nullptr);
        if (!text.empty()) text.pop_back();
        return text;
    }
    char fallback[16]{}; sprintf_s(fallback, "0x%02X", key); return fallback;
}
