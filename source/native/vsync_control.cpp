#include "vsync_control.h"

#include <dxgi1_6.h>
#include <atomic>
#include <mutex>

namespace vsync_control
{
namespace
{
std::atomic<uint32_t> gMode{static_cast<uint32_t>(Mode::eGame)};
std::atomic<uint64_t> gGeneration{0};
std::atomic<bool> gVulkanHook{false};
std::mutex gMutex;
Snapshot gSnapshot;

// The application's latest Vulkan swapchain creation. Its present mode
// outlives requests: only the game's next swapchain changes it.
struct VulkanSwapchain
{
    bool valid = false;
    uint64_t handle = 0;
    uint32_t originalPresentMode = 0;
    uint32_t submittedPresentMode = 0;
    uint64_t configurationGeneration = 0;
    Failure failure = Failure::eNone;
    int32_t result = 0;
};
VulkanSwapchain gVulkan;

bool IsOff(uint32_t mode) noexcept
{
    return mode == static_cast<uint32_t>(Mode::eOff);
}

bool WithoutVsync(uint32_t presentMode) noexcept
{
    return presentMode == kPresentModeImmediate || presentMode == kPresentModeMailbox;
}

Failure IdleFailure(uint32_t mode) noexcept
{
    // Nothing presented under this request yet. On is not offered.
    return mode == static_cast<uint32_t>(Mode::eOn)
        ? Failure::eRuntimeUnavailable : Failure::eNoApplicationSwapchain;
}

// gMutex held: describe the live Vulkan swapchain against the current request.
void RefreshVulkanLocked() noexcept
{
    gSnapshot.vulkanHook = gVulkanHook.load(std::memory_order_acquire);
    if (!gVulkan.valid)
        return;
    const bool off = IsOff(gSnapshot.requestedMode);
    const bool created = gVulkan.result >= 0;
    const bool overridden = created && gVulkan.submittedPresentMode != gVulkan.originalPresentMode;
    const bool current = gVulkan.configurationGeneration == gSnapshot.configurationGeneration;
    gSnapshot.vulkanSwapchain = true;
    gSnapshot.originalInterval = gSnapshot.submittedInterval = 0;
    gSnapshot.originalPresentMode = gVulkan.originalPresentMode;
    gSnapshot.submittedPresentMode = gVulkan.submittedPresentMode;
    gSnapshot.presentResult = gVulkan.result;
    gSnapshot.matched = off && created && WithoutVsync(gVulkan.submittedPresentMode);
    gSnapshot.overrideApplied = gSnapshot.matched && overridden;
    if (!off)
        // A swapchain created under an earlier Off request keeps its mode.
        gSnapshot.failure = overridden ? Failure::eSwapchainRecreationPending
            : gSnapshot.requestedMode == static_cast<uint32_t>(Mode::eOn)
                ? Failure::eRuntimeUnavailable : Failure::eNone;
    else if (gSnapshot.matched)
        gSnapshot.failure = Failure::eNone;
    else if (!current)
        gSnapshot.failure = Failure::eSwapchainRecreationPending;
    else if (!created)
        gSnapshot.failure = Failure::eSwapchainCreateFailed;
    else
        gSnapshot.failure = gVulkan.failure;
    gSnapshot.available = gSnapshot.matched
        || (off && gSnapshot.failure == Failure::eSwapchainRecreationPending);
}
}

void Configure(uint32_t mode) noexcept
{
    mode = mode <= static_cast<uint32_t>(Mode::eOn) ? mode : 0;
    if (gMode.load(std::memory_order_acquire) == mode
        && gGeneration.load(std::memory_order_acquire) != 0)
        return;
    std::lock_guard lock(gMutex);
    gMode.store(mode, std::memory_order_release);
    const uint64_t generation = gGeneration.fetch_add(1, std::memory_order_acq_rel) + 1;
    gSnapshot = {};
    gSnapshot.requestedMode = mode;
    gSnapshot.configurationGeneration = generation;
    gSnapshot.failure = IdleFailure(mode);
    RefreshVulkanLocked();
}

Snapshot ReadSnapshot() noexcept
{
    std::lock_guard lock(gMutex);
    gSnapshot.vulkanHook = gVulkanHook.load(std::memory_order_acquire);
    return gSnapshot;
}

Submission BeforeApplicationPresent(uint32_t interval, uint32_t flags,
    bool principal) noexcept
{
    Submission submission{};
    submission.interval = submission.originalInterval = interval;
    submission.configurationGeneration = gGeneration.load(std::memory_order_acquire);
    if (flags & DXGI_PRESENT_TEST)
        return submission;
    if (IsOff(gMode.load(std::memory_order_acquire)))
        submission.interval = 0;
    submission.changed = submission.interval != interval;
    submission.report = principal;
    return submission;
}

void AfterApplicationPresent(const Submission& submission, HRESULT result) noexcept
{
    if (!submission.report)
        return;
    std::lock_guard lock(gMutex);
    // A newer request replaced the one this Present carried.
    if (submission.configurationGeneration != gSnapshot.configurationGeneration)
        return;
    const bool off = IsOff(gSnapshot.requestedMode);
    gSnapshot.vulkanSwapchain = false;
    gSnapshot.presentResult = result;
    gSnapshot.originalInterval = submission.originalInterval;
    gSnapshot.submittedInterval = submission.interval;
    gSnapshot.available = off;
    gSnapshot.matched = off && SUCCEEDED(result);
    gSnapshot.overrideApplied = gSnapshot.matched && submission.changed;
    if (!off)
        gSnapshot.failure = gSnapshot.requestedMode == static_cast<uint32_t>(Mode::eOn)
            ? Failure::eRuntimeUnavailable : Failure::eNone;
    else
        gSnapshot.failure = SUCCEEDED(result) ? Failure::eNone : Failure::ePresentFailed;
}

bool OffRequested() noexcept
{
    return IsOff(gMode.load(std::memory_order_acquire));
}

void VulkanSwapchainHookInstalled() noexcept
{
    gVulkanHook.store(true, std::memory_order_release);
}

VulkanSwapchainSubmission BeforeVulkanSwapchain(uint32_t presentMode,
    const VulkanSurfaceModes& modes) noexcept
{
    std::lock_guard lock(gMutex);
    VulkanSwapchainSubmission submission{};
    submission.presentMode = submission.originalPresentMode = presentMode;
    submission.configurationGeneration = gSnapshot.configurationGeneration;
    // Game keeps every mode; IMMEDIATE and MAILBOX already present without V-Sync.
    if (!IsOff(gSnapshot.requestedMode) || WithoutVsync(presentMode))
        return submission;
    // Only the two FIFO modes are replaced, and only with a mode the surface
    // lists for an unconstrained creation chain.
    if ((presentMode != kPresentModeFifo && presentMode != kPresentModeFifoRelaxed)
        || !modes.queried || modes.unsupported)
        submission.failure = Failure::eSwapchainUnsupported;
    else if (modes.immediate)
        submission.presentMode = kPresentModeImmediate;
    else if (modes.mailbox)
        submission.presentMode = kPresentModeMailbox;
    else
        submission.failure = Failure::ePresentModeUnavailable;
    submission.changed = submission.presentMode != presentMode;
    return submission;
}

void AfterVulkanSwapchain(const VulkanSwapchainSubmission& submission,
    int32_t result, uint64_t swapchain) noexcept
{
    std::lock_guard lock(gMutex);
    gVulkan.valid = true;
    gVulkan.handle = result >= 0 ? swapchain : 0;
    gVulkan.originalPresentMode = submission.originalPresentMode;
    gVulkan.submittedPresentMode = submission.presentMode;
    gVulkan.configurationGeneration = submission.configurationGeneration;
    gVulkan.failure = submission.failure;
    gVulkan.result = result;
    RefreshVulkanLocked();
}

void VulkanSwapchainDestroyed(uint64_t swapchain) noexcept
{
    if (!swapchain)
        return;
    std::lock_guard lock(gMutex);
    if (!gVulkan.valid || gVulkan.handle != swapchain)
        return;
    gVulkan = {};
    gSnapshot.vulkanSwapchain = gSnapshot.available = false;
    gSnapshot.matched = gSnapshot.overrideApplied = false;
    gSnapshot.originalPresentMode = gSnapshot.submittedPresentMode = 0;
    gSnapshot.presentResult = 0;
    gSnapshot.failure = IdleFailure(gSnapshot.requestedMode);
}
}
