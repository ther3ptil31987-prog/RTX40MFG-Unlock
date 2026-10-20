#pragma once
#include <Windows.h>
#include <cstdint>
#include <cwchar>
#include <iterator>

// Opt-in GPU name override: the menu's "Game FG option" toggle on RTX 30, saved as
// [Compatibility] GpuNameOverride=1 in RTX40MFG-UI.ini beside the game
// executable. Some games decide by adapter name whether to offer
// DLSS Frame Generation; Wuthering Waves removes the option on RTX 30 even though
// Streamline reports DLSS-G as supported there. When enabled, DXGI adapter
// descriptions (GetDesc/GetDesc1/GetDesc2/GetDesc3) that the game executable
// itself asks for report the RTX 40 identity OptiScaler's DXGI spoofing uses by
// default, but only for an NVIDIA GeForce RTX 30 adapter. Vendor, LUID, memory
// sizes, subsystem and revision stay real. Streamline, NGX, the driver, every
// other module and this DLL keep the real description. Off by default; takes
// effect at the next game start.
namespace gpu_name_override
{
constexpr wchar_t kSection[] = L"Compatibility";
constexpr wchar_t kKey[] = L"GpuNameOverride";
constexpr wchar_t kReportedName[] = L"NVIDIA GeForce RTX 4090";
constexpr UINT kReportedDeviceId = 0x2684;
constexpr UINT kNvidiaVendorId = 0x10DE;

enum class Setting { eOff, eOn, eInvalid };

// Missing file or key, 0, false and off are off; 1, true and on are on (any case).
Setting ReadSetting(const wchar_t* iniPath) noexcept;

// An NVIDIA adapter whose real description names a GeForce RTX 20/30 card.
bool Eligible(UINT vendorId, const wchar_t* description) noexcept;

// Only the name and device ID change.
template<class Desc> void Apply(Desc& desc) noexcept
{
    desc.DeviceId = kReportedDeviceId;
    wmemset(desc.Description, L'\0', std::size(desc.Description));
    wcsncpy_s(desc.Description, kReportedName, _TRUNCATE);
}

struct State
{
    bool enabled = false;
    bool armed = false;
    uint32_t methods = 0;      // bit 0 GetDesc, 1 GetDesc1, 2 GetDesc2, 3 GetDesc3
    uint64_t reported = 0;     // game queries answered with the RTX 40 identity
    uint64_t unchanged = 0;    // game queries for a hardware adapter that is not RTX 20/30
};
State Read() noexcept;

// Patch worker, never under the loader lock: reads the setting and, when it is
// on, installs the adapter method detours before the game reads its GPU name.
void ArmFromSettings() noexcept;

// Menu: the saved setting beside the game executable, and saving a change. A
// change applies at the next game start; Read().enabled is this session's value.
bool SavedOn() noexcept;
bool Save(bool on) noexcept;

// Installs the detours on the System32 dxgi.dll adapter methods. Idempotent.
bool Arm() noexcept;

#if MFG_GPU_NAME_OVERRIDE_TESTING
namespace testing
{
// Replaces the "RTX 30" name rule so the detours can be exercised on another GPU.
void SetEligibleToken(const wchar_t* token) noexcept;
}
#endif
}
