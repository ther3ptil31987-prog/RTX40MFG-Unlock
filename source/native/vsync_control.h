#pragma once

#include <Windows.h>
#include <cstdint>

// Optional V-Sync Off override for the application's own Presents.
//
// Off submits the application's top-level Present/Present1 calls on the menu's
// swapchain proxy with sync interval 0, above Streamline's proxy, so DLSS-G
// presents its frames without V-Sync as well. Frame generation with V-Sync off
// is NVIDIA's standard configuration. Game leaves every argument untouched. On
// (2, saved by older builds) is not offered and behaves as Game. Presentation
// flags are never changed, TEST presents are never changed, and NVIDIA Control
// Panel V-Sync overrides still take priority in the driver.
namespace vsync_control
{
enum class Mode : uint32_t { eGame = 0, eOff = 1, eOn = 2 };

enum class Failure : uint32_t
{
    eNone = 0,
    eRuntimeUnavailable,
    eNoApplicationSwapchain,
    eInterposerUnverified,
    eWrongOwner,
    eTargetChanged,
    eDeviceIdentityLost,
    eNotPrincipal,
    eNestedPresent,
    eTestPresent,
    eGenerationChanged,
    ePresentFailed,
    eConcurrentPresent,
};

struct Snapshot
{
    uint32_t requestedMode = 0;
    // The Off request can be carried out: a principal application swapchain
    // on the menu's proxy presented while Off was requested.
    bool available = false;
    // The last principal application Present carried the request and DXGI
    // accepted it. This does not observe an NVIDIA Control Panel override.
    bool matched = false;
    // That Present's interval was actually changed.
    bool overrideApplied = false;
    uint32_t originalInterval = 0;
    uint32_t submittedInterval = 0;
    Failure failure = Failure::eNoApplicationSwapchain;
    int32_t presentResult = 0;
    uint64_t configurationGeneration = 0;
};

// Worker: the saved mode (0 game, 1 off, 2 on). Cheap when unchanged.
void Configure(uint32_t mode) noexcept;
Snapshot ReadSnapshot() noexcept;

inline const char* FailureName(Failure failure) noexcept
{
    switch (failure)
    {
    case Failure::eNone: return "none";
    case Failure::eRuntimeUnavailable: return "runtime-unavailable";
    case Failure::eNoApplicationSwapchain: return "application-swapchain-unproven";
    case Failure::eInterposerUnverified: return "interposer-unverified";
    case Failure::eWrongOwner: return "creator-owner-mismatch";
    case Failure::eTargetChanged: return "present-target-changed";
    case Failure::eDeviceIdentityLost: return "device-identity-lost";
    case Failure::eNotPrincipal: return "principal-swapchain-unproven";
    case Failure::eNestedPresent: return "nested-present";
    case Failure::eTestPresent: return "test-present";
    case Failure::eGenerationChanged: return "generation-changed";
    case Failure::ePresentFailed: return "present-not-accepted";
    case Failure::eConcurrentPresent: return "concurrent-present";
    }
    return "unknown";
}

struct Submission
{
    uint32_t interval = 0;
    uint32_t originalInterval = 0;
    uint64_t configurationGeneration = 0;
    bool changed = false;
    bool report = false;
};

// Menu swapchain proxy, top-level (non-nested) application Present only.
// principal: this is the chain the menu treats as the game's main output; only
// that chain updates the status.
Submission BeforeApplicationPresent(uint32_t interval, uint32_t flags,
    bool principal) noexcept;
void AfterApplicationPresent(const Submission& submission, HRESULT result) noexcept;
}
