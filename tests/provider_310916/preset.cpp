// Frame Generation Preset C on DLSS-G 310.9.16: compiles the production dlssg_preset.cpp and ngx_mfg_gate.h
// against providers mapped with DONT_RESOLVE_DLL_REFERENCES (no DllMain, no imports, no provider code runs).
// entry_detour is stubbed so nothing is installed.
#include "dlssg_preset.cpp"
#include "ngx_mfg_gate.h"
#include "data_image.h"

namespace entry_detour
{
bool Install(Kind, HMODULE, void*, void*, void*& originalTrampoline, const InstallOptions&, Handle*) noexcept
{
    originalTrampoline = nullptr;
    return false;
}
bool Install(Kind, HMODULE, void*, void*, void*& originalTrampoline) noexcept
{
    originalTrampoline = nullptr;
    return false;
}
bool InstallForwarding(Kind, HMODULE, void*, ForwardPreCall, void*& originalTrampoline, const InstallOptions&,
    Handle*, ForwardGate, void*) noexcept
{
    originalTrampoline = nullptr;
    return false;
}
Snapshot ReadSnapshot(Handle) noexcept { return {}; }
Snapshot ReadSnapshot(Kind) noexcept { return {}; }
Snapshot ReadSnapshot(Kind, HMODULE) noexcept { return {}; }
}

int wmain(int argc, wchar_t** argv)
{
    using namespace dlssg_preset;
    namespace policy = dlssg_provider_policy;
    try
    {
        if (argc != 3) throw std::runtime_error("usage: audit 310.9.0.dll 310.9.16.dll");
        Require(IsValidSelection(kPresetC) && IsOverride(kPresetC) && !IsValidSelection(4), "Preset C is a valid selection");
        Setting setting{};
        setting.version = kSettingVersion;
        setting.id = kSettingId;
        Require(OverrideRead(kSettingId, kSettingNotFound, setting, kPresetC) && setting.current.dword == 3,
            "Preset C override supplies driver value 3");

        const HMODULE oldProvider = LoadLibraryExW(argv[1], nullptr, DONT_RESOLVE_DLL_REFERENCES);
        const HMODULE newProvider = LoadLibraryExW(argv[2], nullptr, DONT_RESOLVE_DLL_REFERENCES);
        Require(oldProvider && newProvider, "providers mapped without running their code");
        void* const oldReader = FindCachedReader(oldProvider);
        void* const newReader = FindCachedReader(newProvider);
        std::cout << std::hex << "reader_rva_310_9_0=0x" << reinterpret_cast<uintptr_t>(oldReader) - reinterpret_cast<uintptr_t>(oldProvider)
            << " reader_rva_310_9_16=0x" << reinterpret_cast<uintptr_t>(newReader) - reinterpret_cast<uintptr_t>(newProvider)
            << std::dec << '\n';
        Require(oldReader && reinterpret_cast<uintptr_t>(oldReader) - reinterpret_cast<uintptr_t>(oldProvider) == 0x13F00,
            "310.9.0 cached preset reader (one call site)");
        Require(newReader && reinterpret_cast<uintptr_t>(newReader) - reinterpret_cast<uintptr_t>(newProvider) == 0x14000,
            "310.9.16 cached preset reader (two call sites, one reader)");

        Require(policy::IsSupportedRetainedProvider(newProvider) && policy::IsSupportedRetainedProvider(oldProvider),
            "both providers eligible on Ada");
        Require(policy::IsSupportedRetainedAmpereProvider(newProvider) && policy::IsSupportedRetainedAmpereProvider(oldProvider),
            "RTX 30 admits 310.9.16 and 310.9.0");
        Require(policy::IsSupportedAmpereProvider(newProvider, argv[2]) && policy::IsSupportedAmpereProvider(oldProvider, argv[1]),
            "RTX 30 path check matches");
        Require(EffectiveSelection(kPresetC, newProvider) == kPresetC && EffectiveSelection(kPresetC, oldProvider) == kGameOrDriver,
            "the RTX 30 preset hook applies the same Preset C rule");
        Require(PresetCSupported(newProvider) && !PresetCSupported(oldProvider), "Preset C support follows the provider version");
        Require(EffectivePreset(kPresetC, newProvider) == kPresetC && EffectivePreset(kPresetC, oldProvider) == kGameOrDriver,
            "Preset C reaches 310.9.16 only; elsewhere the game/driver value stays");
        Require(EffectivePreset(kPresetA, oldProvider) == kPresetA && EffectivePreset(kPresetB, newProvider) == kPresetB
            && EffectivePreset(kGameOrDriver, newProvider) == kGameOrDriver, "A, B and game/driver unchanged");
        Require(ReadSnapshot(newProvider).presetCAvailable && !ReadSnapshot(oldProvider).presetCAvailable,
            "status reports Preset C availability per provider");

        const auto oldGate = ngx_mfg_gate::Find(oldProvider);
        const auto newGate = ngx_mfg_gate::Find(newProvider);
        std::cout << std::hex << "mfg_gate_rva_310_9_0=0x" << reinterpret_cast<uintptr_t>(oldGate.match) - reinterpret_cast<uintptr_t>(oldProvider)
            << " mfg_gate_rva_310_9_16=0x" << reinterpret_cast<uintptr_t>(newGate.match) - reinterpret_cast<uintptr_t>(newProvider)
            << std::dec << '\n';
        Require(oldGate.match && newGate.match, "RTX 40 MFG count gate found once in both providers");
        std::cout << "PRESET_C_310916_AUDIT_PASSED\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << "AUDIT_FAILED: " << error.what() << '\n';
        return 1;
    }
}
