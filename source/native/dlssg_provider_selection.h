#pragma once
#include "dlssg_provider_policy.h"
#include <memory>
#include <string>

namespace dlssg_provider_selection
{
struct Candidate
{
    std::wstring path;
    dlssg_provider_policy::VersionTriplet version{};
    bool nvidiaSigned = false;
    explicit operator bool() const noexcept { return !path.empty(); }
};
// Inspect an ordinary file without executing it. Export identity is established
// before embedded-version eligibility. Sibling providers never enter selection.
bool Inspect(const std::wstring& path, Candidate& candidate) noexcept;
bool InspectNvidiaSigned(const std::wstring& path, Candidate& candidate) noexcept;
// Production discovery has one fixed OTA root. An empty plan preserves every
// argument of the game's native load; it never searches for a replacement game.
void Prepare(const std::wstring& executableDirectory) noexcept;
#if defined(MFG_PROVIDER_SELECTION_TESTING)
// Test executable only: use the production traversal on an isolated fixture
// tree. This entry point and root override are absent from the shipped DLL.
void PrepareForTests(const std::wstring& executableDirectory, const std::wstring& versionsDirectory) noexcept;
bool NvidiaSignatureForTests(const std::wstring& path) noexcept;
#endif
struct LoadPlan
{
    std::wstring path;
    std::shared_ptr<void> fileLease; // Denies replacement while the loader maps it.
    dlssg_provider_policy::VersionTriplet version{};
    bool nvidiaSigned = false;
    explicit operator bool() const noexcept { return !path.empty(); }
};
LoadPlan Plan(const std::wstring& requested, HANDLE file, DWORD flags, bool beforeFirstCreate) noexcept;
// Called on loader return; validates the actual mapped owner, not the filename.
bool Loaded(const LoadPlan& plan, HMODULE module) noexcept;
struct Snapshot
{
    Candidate selected;
    uint64_t attempts = 0, loaded = 0, failures = 0;
    uint32_t examined = 0, signatureRejections = 0;
    bool prepared = false, scanLimited = false, nativeProviderPinned = false;
};
Snapshot Read() noexcept;
}
