#pragma once
#include <algorithm>
#include <array>
#include <cstdint>

namespace sdk_pacing::cadence
{
// The DLSS-G CPU pacer's interval, from sl.dlss_g 2.14.1:
// - interval = mean of the last six batch-start deltas / outputs per batch (0x3C9F0);
// - output i targets batchStart + i*interval;
// - no output waits more than one interval from "now" (0x49A15-0x49C56).
// The batch start is the SDK's own base for that frame, stamped once output 0's
// present fence completes, so output 0 is never delayed by this estimate. The
// interval therefore follows produced cadence, not this pacer's own output.
// A discontinuity is treated like the pacer's state-change reset (0x4A920):
// a frame gap or a delta outside 1-250 ms.
// V1.5.1-beta.3 (Dynamic MFG): a live count change between consecutive frames keeps
// the history. With fewer outputs the deltas are scaled by new/old outputs, keeping
// the time per output, so a presented rate held by a frame limit lets the real frame
// rate rise to the lower count. Reset, the next batches were paced at the old capped
// real interval, which back-pressure then held: the step down could never take effect.
// With more outputs the deltas stay, so no batch takes longer than before.
// V1.6.0 hitch.1: the plain mean let one game hitch (a 50-250 ms frame: shader
// compilation, streaming) into the next six batches. At 3x and up the real frame of
// each of them was then held past the base frame time, the game thread waited for it,
// and the following deltas grew too; the driver's flip metering got the same mean as
// its batch time. Stock XeFG takes the median of nine here. Now a delta over 3/2 of
// the window's median stays in the window (it moves the median, and a lasting slowdown
// is taken within three batches) but is left out of the mean. A gap of up to three
// frames without generated outputs (an XeFG reset, a scene cut, a Dynamic MFG change)
// keeps the history instead of starting over.
// V1.6.0 latency.1: XeFG's batch start is a GPU completion time (output 0's present fence),
// while a deadline is the time its Present is called. XeFG's own deadline (0x224B30)
// subtracts its smoothed present-call -> GPU-completion latency (record+0x14, 0x21F230);
// without it every output, the real frame included, completed that latency late (up to
// one interval: +16 ms click-to-photon at XeFG 2x with XeLL, 55 vs 38 ms without us).
class BatchIntervalEstimator
{
public:
    static constexpr uint32_t kSamples = 6;
    static constexpr int64_t kMinimumDelta = 1000000;
    static constexpr int64_t kMaximumDelta = 250000000;
    // hitch.1: a delta above kOutlierNumerator/kOutlierDenominator of the window median is not averaged.
    static constexpr int64_t kOutlierNumerator = 3, kOutlierDenominator = 2;
    // hitch.1: frames without generated outputs that may lie between two observed batches.
    static constexpr uint32_t kBridgeFrames = 3;
    struct Decision { int64_t deadline{}, interval{}; uint32_t samples{}; bool capped{}; int64_t lead{}; };
    // What Observe did with a batch start (hitch.1 log).
    enum class Event : uint8_t { kRepeat, kStart, kSample, kBridge, kResetGap, kResetRange, kResetBase };

    void Reset() noexcept { *this = {}; }
    // Called once per batch with that frame's base. Repeats of the same batch are ignored.
    Event Observe(uint32_t frame, uint32_t outputs, int64_t batchStart) noexcept
    {
        if (outputs < 2 || batchStart <= 0) { Reset(); return Event::kResetBase; }
        if (started_ && frame == frame_ && outputs == outputs_ && batchStart == start_) return Event::kRepeat;
        int64_t delta = batchStart - start_;
        const uint32_t gap = frame - frame_;
        rawDelta_ = started_ ? delta : 0; gap_ = started_ ? gap : 0;
        const bool continuous = started_ && gap == 1 && delta >= kMinimumDelta && delta <= kMaximumDelta;
        const bool bridged = started_ && count_ && gap >= 2 && gap <= kBridgeFrames+1
            && delta >= kMinimumDelta*int64_t(gap) && delta <= kMaximumDelta*int64_t(gap);
        Event event = continuous ? Event::kSample : bridged ? Event::kBridge
            : !started_ ? Event::kStart : gap != 1 ? Event::kResetGap : Event::kResetRange;
        lastDelta_ = 0;
        if (!continuous && !bridged) { deltas_ = {}; count_ = 0; cursor_ = 0; }
        else
        {
            if (outputs < outputs_)
            {
                // The deltas measured so far, and this one, were paced for the old count.
                for (uint32_t i = 0; i < count_; ++i) deltas_[i] = deltas_[i] * int64_t(outputs) / int64_t(outputs_);
                delta = delta * int64_t(outputs) / int64_t(outputs_);
            }
            // A bridged gap spans frames without generated outputs: not a batch delta.
            if (continuous)
            {
                if (count_ < kSamples) ++count_;
                deltas_[cursor_] = delta;
                cursor_ = (cursor_ + 1) % kSamples;
                lastDelta_ = delta;
            }
        }
        started_ = true; frame_ = frame; outputs_ = outputs; start_ = batchStart;
        Summarize();
        return event;
    }
    // Index 1..outputs-1: generated outputs, then the real frame last. latency.1: `lead` (XeFG's
    // present latency) moves the deadline earlier, never before the batch start.
    bool Plan(uint32_t frame, uint32_t outputs, uint32_t index, int64_t now, Decision& decision,
        int64_t lead = 0) const noexcept
    {
        if (!started_ || !count_ || frame != frame_ || outputs != outputs_ || index < 1 || index >= outputs || now <= 0)
            return false;
        const int64_t interval = typical_ / int64_t(outputs_);
        if (interval <= 0) return false;
        const int64_t span = interval * int64_t(index);
        const int64_t used = std::clamp<int64_t>(lead, 0, span);
        int64_t deadline = start_ + span - used;
        const bool capped = deadline > now + interval;
        if (capped) deadline = now + interval;
        decision = {deadline, interval, count_, capped, used};
        return true;
    }
    uint32_t Samples() const noexcept { return count_; }
    // flipmeter.1: the time a batch of `outputs` presents takes at this estimate, for the driver's
    // flip metering before that batch's first Present (before its own start is observed): the
    // estimate (hitch.1: without hitches), scaled to the same time per output for fewer outputs
    // than measured, as Observe does.
    int64_t BatchTime(uint32_t outputs) const noexcept
    {
        if (!started_ || !count_ || !outputs_ || outputs < 2) return 0;
        return outputs < outputs_ ? typical_ * int64_t(outputs) / int64_t(outputs_) : typical_;
    }
    // hitch.1 (log): the window's median, its plain mean (V1.5.1's estimate), the estimate used,
    // the deltas left out of it, and the delta the last Observe sampled (0 when it sampled none).
    int64_t Median() const noexcept { return median_; }
    int64_t Mean() const noexcept { return mean_; }
    int64_t Typical() const noexcept { return typical_; }
    uint32_t Excluded() const noexcept { return excluded_; }
    int64_t LastDelta() const noexcept { return lastDelta_; }
    uint32_t Outputs() const noexcept { return outputs_; }
    // The last Observe's batch-start difference and frame gap as seen (a reset's too; 0 at a start).
    int64_t RawDelta() const noexcept { return rawDelta_; }
    uint32_t Gap() const noexcept { return gap_; }

private:
    void Summarize() noexcept
    {
        median_ = mean_ = typical_ = 0; excluded_ = 0;
        if (!count_) return;
        std::array<int64_t, kSamples> sorted{};
        std::copy_n(deltas_.begin(), count_, sorted.begin());
        std::sort(sorted.begin(), sorted.begin()+count_);
        median_ = count_ % 2 ? sorted[count_/2] : (sorted[count_/2-1] + sorted[count_/2]) / 2;
        int64_t sum{}, kept{};
        uint32_t used{};
        for (uint32_t i = 0; i < count_; ++i)
        {
            sum += sorted[i];
            if (sorted[i]*kOutlierDenominator <= median_*kOutlierNumerator) { kept += sorted[i]; ++used; }
        }
        mean_ = sum / int64_t(count_);
        typical_ = kept / int64_t(used); // the lower half is always within bounds of its own median
        excluded_ = count_ - used;
    }
    // While fewer than kSamples are held they fill [0, count_).
    std::array<int64_t, kSamples> deltas_{};
    int64_t start_{}, median_{}, mean_{}, typical_{}, lastDelta_{}, rawDelta_{};
    uint32_t count_{}, cursor_{}, frame_{}, outputs_{}, excluded_{}, gap_{};
    bool started_{};
};

// flipmeter.1: a deadline inside a batch the NVIDIA driver meters. The display hardware spaces the
// batch's flips at timePerBatch / outputs from its first flip but never holds that first one, so
// output `index` (1..outputs-1) flips at batchStart + index * spacing; it is released `lead` early
// (at most half a spacing) so the presenting thread's wake-up, copy and Present no longer place it.
// No output waits more than one spacing from now (the DLSS-G cap above).
struct MeteredDeadline { int64_t deadline{}, lead{}; bool capped{}; };
inline bool PlanMetered(int64_t batchStart, int64_t spacing, uint32_t index, int64_t lead, int64_t now,
    MeteredDeadline& decision) noexcept
{
    if (batchStart <= 0 || spacing <= 0 || index < 1 || lead < 0 || now <= 0) return false;
    const int64_t used = lead < spacing / 2 ? lead : spacing / 2;
    int64_t deadline = batchStart + spacing * int64_t(index) - used;
    const bool capped = deadline > now + spacing;
    if (capped) deadline = now + spacing;
    decision = {deadline, used, capped};
    return true;
}
}
