#pragma once

#include <Windows.h>
#include <cstdint>

namespace dlssg_preset
{
struct SelectionSnapshot
{
    uint32_t requested = 2;
    uint32_t latched = 2;
    bool selectionFrozen = false;
    bool restartRequired = false;
};

struct Snapshot
{
    bool installed = false;
    uint32_t failure = 0;
    uint64_t providerReads = 0;
    bool cachedReaderInstalled = false;
    uint32_t cachedReaderFailure = 0;
    uint32_t cachedReaderRva = 0;
    uint64_t cachedReaderReads = 0;
    uint32_t requested = 2;
    uint32_t latched = 2;
    bool selectionFrozen = false;
    bool restartRequired = false;
    uint32_t observedPreset = 0;
    bool observedPresetValid = false;
    uint64_t overrideReads = 0;
    // The provider implements Preset C (DLSS-G 310.9.16 and newer).
    bool presetCAvailable = false;
};

// 0 leaves game/driver reads untouched; 1 selects A, 2 selects B and 3 selects C
// on providers that implement it (elsewhere C leaves the game/driver value). A process
// keeps one selection after its first relevant provider query. Later requests
// are saved for restart, without replacing cached settings or live programs.
bool SetRequested(uint32_t preset) noexcept;
uint32_t FreezeSelection() noexcept;
// The selection a provider receives: C becomes the game/driver value (0) on providers without it.
uint32_t EffectiveSelection(uint32_t preset, HMODULE provider) noexcept;
SelectionSnapshot ReadSelection() noexcept;
using InitialSelectionLoader = uint32_t (*)() noexcept;
// Assign before enabling hooks. Only FreezeSelection calls this loader, once,
// outside the state lock and only if SetRequested has not already initialized it.
void SetInitialSelectionLoader(InitialSelectionLoader loader) noexcept;
// Query-result evidence only. The caller owns a validated, process-retained
// exact provider route and has established output-buffer ownership.
void RecordProviderRead(HMODULE provider, bool valueValid, uint32_t value, bool overridden) noexcept;

// Call at a normal initialization/API boundary, outside DllMain. Installs only
// a process-memory read hook; no NVIDIA profile set/save API is used.
bool Prepare() noexcept;
void PrepareForExport(HMODULE module, const char* name) noexcept;
// Bounded entry discovery; no NVIDIA initialization is invoked here.
void ObserveProvider(HMODULE module, uint64_t generation) noexcept;
// The provider's FG preset reader: every decoded preset-key call site must call this one
// reader (known prologue, at its unwind entry). Null when absent or when sites disagree.
void* FindPresetReader(HMODULE module) noexcept;
Snapshot ReadSnapshot(HMODULE provider) noexcept;
}
