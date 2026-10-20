#pragma once
#include <algorithm>
#include <cstdint>

namespace frame_telemetry
{
struct PresentDelta
{
    uint32_t frames = 0;
    bool resetWindow = false;
    bool available = false;
};

// Sample the cumulative DXGI count, including native presents made between
// application callbacks. The owner is a lifetime generation, never a pointer.
class PresentCounter
{
public:
    PresentDelta Sample(uint64_t owner, uint32_t count,
        uint64_t tick, bool available) noexcept
    {
        const bool discontinuity = owner != owner_ || tick < tick_
            || tick - tick_ > 2000;
        owner_ = owner;
        tick_ = tick;
        if (!available || owner == 0)
        {
            const bool reset = haveCount_ || discontinuity;
            haveCount_ = false;
            return {0, reset, false};
        }
        const uint32_t delta = count - count_; // UINT wrap is intentional.
        const bool reset = !haveCount_ || discontinuity || delta >= 0x80000000u;
        count_ = count;
        haveCount_ = true;
        return {reset ? 0u : delta, reset, true};
    }

private:
    uint64_t owner_ = 0;
    uint64_t tick_ = 0;
    uint32_t count_ = 0;
    bool haveCount_ = false;
};

inline uint32_t RateMilli(uint64_t frames, uint64_t frequency,
    uint64_t elapsedTicks) noexcept
{
    if (!frequency || !elapsedTicks) return 0;
    const long double value = static_cast<long double>(frames)
        * frequency * 1000.0L / elapsedTicks;
    return static_cast<uint32_t>(std::min<long double>(UINT32_MAX, value + 0.5L));
}

// DLSS-G's own presented-frame count (DLSSGState::numFramesActuallyPresented).
// sl.dlss_g 2.14.1's slGetData copies it from the presentation state and never
// resets it on read: it is the latest presented batch (1 real plus generated
// frames), not a count since the previous query as sl_dlss_g.h describes. Sum
// one sample per real frame, and count the real frames that had none.
struct DlssgPresentedTotals
{
    uint64_t frames = 0;        // summed batches of the sampled real frames
    uint64_t sampledFrames = 0; // real frames that had a sample
    uint64_t gaps = 0;          // real frames without one, and discontinuities
    int64_t lastTicks = 0;      // QueryPerformanceCounter of the latest sample
};

class DlssgPresentedCounter
{
public:
    // realFrame: the monotonic real-frame count when the query returned.
    // Several queries in one real frame read the same batch; the last wins.
    void Sample(uint64_t realFrame, uint32_t presented, int64_t ticks) noexcept
    {
        if (!haveFrame_ || realFrame < frame_)
        {
            if (haveFrame_) ++totals_.gaps;
            haveFrame_ = true;
        }
        else if (realFrame != frame_)
        {
            totals_.frames += value_;
            ++totals_.sampledFrames;
            totals_.gaps += realFrame - frame_ - 1;
        }
        frame_ = realFrame;
        value_ = presented;
        totals_.lastTicks = ticks;
    }
    DlssgPresentedTotals Totals() const noexcept { return totals_; }

private:
    DlssgPresentedTotals totals_{};
    uint64_t frame_ = 0;
    uint32_t value_ = 0;
    bool haveFrame_ = false;
};

// Output rate over a telemetry window, in milli-FPS; 0 when it is not a
// measurement: every real frame of the window must have had a sample, with no
// discontinuity, and the latest sample must be current (250 ms). It never
// falls back to the multiplier.
inline uint32_t DlssgPresentedRateMilli(const DlssgPresentedTotals& start,
    const DlssgPresentedTotals& end, uint64_t realFrames, uint64_t frequency,
    uint64_t elapsedTicks, int64_t nowTicks) noexcept
{
    // A query on another thread may land after the caller read nowTicks.
    const uint64_t age = nowTicks > end.lastTicks ? static_cast<uint64_t>(nowTicks - end.lastTicks) : 0;
    if (!frequency || !elapsedTicks || realFrames < 2 || end.gaps != start.gaps
        || end.sampledFrames < start.sampledFrames || end.frames < start.frames
        || !end.lastTicks || age > frequency / 4)
        return 0;
    const uint64_t sampled = end.sampledFrames - start.sampledFrames;
    // A sample commits when the next real frame begins: allow that one frame.
    if (sampled + 1 < realFrames || sampled > realFrames + 1)
        return 0;
    return RateMilli(end.frames - start.frames, frequency, elapsedTicks);
}

// Internal single-module call; the existing exported no-argument ABI is retained.
void SamplePresentCounter(uint64_t owner, uint32_t count, bool available);
// Vulkan application present: real frames from the interval trace, output from
// DLSS-G's own count (the wrapped present may sit above sl.dlss_g's presents).
void SampleVulkanPresent();
}
