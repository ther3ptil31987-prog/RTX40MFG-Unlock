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
std::mutex gMutex;
Snapshot gSnapshot;

bool OffRequested(uint32_t mode) noexcept
{
    return mode == static_cast<uint32_t>(Mode::eOff);
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
    // Nothing presented under this request yet. On is not offered.
    gSnapshot.failure = mode == static_cast<uint32_t>(Mode::eOn)
        ? Failure::eRuntimeUnavailable : Failure::eNoApplicationSwapchain;
}

Snapshot ReadSnapshot() noexcept
{
    std::lock_guard lock(gMutex);
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
    if (OffRequested(gMode.load(std::memory_order_acquire)))
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
    const bool off = OffRequested(gSnapshot.requestedMode);
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
}
