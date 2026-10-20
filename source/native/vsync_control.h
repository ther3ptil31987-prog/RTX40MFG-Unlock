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
//
// Vulkan fixes the present mode when a swapchain is created. Off replaces a
// FIFO or FIFO_RELAXED mode in the application's vkCreateSwapchainKHR with
// IMMEDIATE, else MAILBOX, only when the surface lists that mode; no other
// creation field changes. A change therefore applies to the game's next
// swapchain, and the status says so until then.
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
    // Vulkan (appended; the status publishes these values).
    eSwapchainRecreationPending,
    ePresentModeUnavailable,
    eSwapchainUnsupported,
    eSwapchainCreateFailed,
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
    // The application's latest live Vulkan swapchain is the presentation the
    // fields above describe; the intervals then stay 0 and the present modes
    // (VkPresentModeKHR values) hold what the game asked for and what was
    // created. presentResult is that creation's VkResult.
    bool vulkanSwapchain = false;
    // The Vulkan swapchain creation wrapper is installed.
    bool vulkanHook = false;
    uint32_t originalPresentMode = 0;
    uint32_t submittedPresentMode = 0;
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
    case Failure::eSwapchainRecreationPending: return "swapchain-recreation-pending";
    case Failure::ePresentModeUnavailable: return "present-mode-unavailable";
    case Failure::eSwapchainUnsupported: return "swapchain-shape-unsupported";
    case Failure::eSwapchainCreateFailed: return "swapchain-create-failed";
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

// Vulkan present modes (VkPresentModeKHR values).
constexpr uint32_t kPresentModeImmediate = 0;
constexpr uint32_t kPresentModeMailbox = 1;
constexpr uint32_t kPresentModeFifo = 2;
constexpr uint32_t kPresentModeFifoRelaxed = 3;

struct VulkanSurfaceModes
{
    // The surface's present modes were read for this creation.
    bool queried = false;
    bool immediate = false;
    bool mailbox = false;
    // The creation chain constrains or lists present modes itself
    // (VkSwapchainPresentModesCreateInfoEXT), or the read failed.
    bool unsupported = false;
};

struct VulkanSwapchainSubmission
{
    uint32_t presentMode = 0;
    uint32_t originalPresentMode = 0;
    uint64_t configurationGeneration = 0;
    Failure failure = Failure::eNone;
    bool changed = false;
};

// The current request is Off: the caller reads the surface's modes only then.
bool OffRequested() noexcept;
void VulkanSwapchainHookInstalled() noexcept;
// The application's vkCreateSwapchainKHR on a tracked device, before and after
// the call down the chain. swapchain is the created handle (0 on failure).
VulkanSwapchainSubmission BeforeVulkanSwapchain(uint32_t presentMode,
    const VulkanSurfaceModes& modes) noexcept;
void AfterVulkanSwapchain(const VulkanSwapchainSubmission& submission,
    int32_t result, uint64_t swapchain) noexcept;
void VulkanSwapchainDestroyed(uint64_t swapchain) noexcept;
}
