#pragma once
#include <algorithm>
#include <array>
#include <cstdint>

namespace sdk_pacing::cadence
{
// V1.6.1 pacing.1: the flip queue's display clock (the dev.25-31 jitter buffer of the standalone route, Witcher 6x on
// the RTX 4080, user: "pretty close" to DLSS-G; source-snapshots\dev32 sdk_pacing_meter.h).
//
// Why: DLSS-G submits a whole batch early into a flip queue about one batch deep and lets the driver time every flip
// (CapFrameX, tester, 20 Oct: DLSS-G presents in bursts of 0.5 ms with a flat display line; XeMFG presents evenly
// spaced by the CPU with a +-0.7 ms display band). Its display passes about half of the game's batch jitter on
// (display-on-production slope 0.48, 8 Oct captures) and keeps ~1 batch of submit -> flip latency. Our CPU pacing
// released each output just in time (2.5 ms before its slot, while XeFG's present -> GPU completion is ~9 ms in a
// GPU-bound game), never held the real -> next first boundary, and with Reflex the queue was empty, so every bit of
// production and copy jitter reached the screen; low base frame rates suffered most.
//
// Each output's Present waits on the GPU for a gate the meter thread signals at Plan()'s release time (the copy is
// queued before the gate, so it is complete when the flip is released). Like a media jitter buffer:
// - rate: per-output interval = smoothed batch hand-over delta / outputs (the game's own cadence, where it hands the
//   batch over, not at our flips). V1.6.1: a delta over kHitchNumerator/kHitchDenominator of the running batch
//   period is a hitch and is not averaged (hitch.1's rule for the CPU pacer; three in a row are a real slowdown and
//   are taken), so one 50-250 ms game frame no longer slows the following batches;
// - phase: each batch's first flip follows the previous flip by the interval, nudged (at most kMaximumCorrection)
//   toward a target hand-over -> first-flip latency, so early or late game frames change the buffer depth instead of
//   the display spacing;
// - a first output that misses its slot flips at readiness and the clock continues from there (no catch-up burst);
// - target: the buffer must cover how long a batch's first output takes to be ready after the game hands the batch
//   over (its GPU work queues behind the game's rendering). When that production latency comes within
//   kProductionMargin of the target, the target jumps to cover it; otherwise it relaxes kRelaxStep per batch. It stays
//   between a fraction of a batch and what the native swap chain can queue ((buffers - outputs - 2) intervals, at
//   least one). A first whose copy only waited for our own previous gate (ready within kGateBoundSlack of its signal)
//   never raises it: that would let the target follow the display (the dev.12 lock-in).
// Lock-in: a meter slower than the game fills the queue. With a deep queue the measured latency then exceeds the
// target and the phase correction speeds the meter up. XeFG's native swap chain usually has the game's 3 buffers, so
// the queue holds about two outputs: the game is then held by the full queue (a gated Present blocks in the native
// Present) and hands each batch over at our own flip cadence, which the rate would learn (simulated: stuck at 42 ms
// per batch against a 25 ms capacity). Such a coupled batch therefore never feeds the rate; it shrinks the interval by
// 1/kCoupledDecay instead, so a held meter drifts down until the game, not the meter, sets the cadence (dev.12's
// "gated decay"); a batch the game hands over late on its own resumes normal rate learning.
// pacing.2 (Subnautica 2, 20 Oct, GPU hang after 3.3 s): the coupling signal fired on every batch (ReShade on the
// native swap chain makes every Present take > 1 ms) while every batch-first output was already late (109 of 110), so
// the interval decayed 3% per batch without bound and the driver was handed a batch time heading for ~1 ms. Now a
// coupled batch decays only while its first output is still on time against the running clock (a late first proves
// the queue was empty: the game was not held by it, so its hand-over is an ordinary sample), never below
// 1/kCoupledFloor of the last uncoupled rate, and never below kMinimumInterval.
// V1.6.2-alpha.2 (tester log, God of War Ragnarok, RTX 30, OptiScaler as dxgi.dll so XeFG's chain kept 3 buffers, 5X,
// DLSS 5 on: "much less smooth", V1.6.1 fine): 5 outputs never fit 3 buffers, so a gated Present blocked on 1,620 of
// 2,556 batches; each decayed the clock until true hand-overs (a steady 43.75 ms) looked like hitches against it (190),
// a sawtooth of 4.2-8.8 ms per output: four flips 5-6 ms apart, then a 20-24 ms hole. A coupled batch now decays only
// while its hand-over follows the cadence we flipped the previous batch at (effective interval x its outputs, within
// kFollowTolerance); a slower hand-over means the game, GPU or display sets the cadence, not our queue, and it is an
// ordinary rate sample. The target was capped at one interval
// there while the first output took 15-23 ms to produce, so the phase correction sped the clock up 4% on every batch;
// the capacity bounds the queue time after readiness, so the target may now reach the production latency plus it.
// V1.6.2 lockin.1 (Wuthering Waves tester log, RTX 5070 Ti, 3X, 8 native buffers, output ring 2, 22 Oct: interval 9 ->
// 15 -> 57.6 ms per output, latency 82 -> 348 ms, phase correction pinned at +4%, coupled 0 while coupled_idle rose
// 4 -> 58, ~20 s to recover): the game was held by our held outputs, but the coupling signal never fired (vsync.2 counts
// a block only when the presenter was backlogged) and every hand-over, at our own cadence or slower, fed the rate. Two
// signs now prove the meter is behind the game: `full` (a Present blocked behind more than a whole batch of gates, as
// only a native latency above the batch's outputs allows; the GoW 3-buffer case stays on the backlog rule), and a
// buffer at least half a batch above the target (phase correction >= kDeepCorrection) kDeepRun batches in a row: the
// output ring's wait on the game queue holds the game that way without any blocked Present (simulated: interval 47 ->
// 7 ms took 540 batches at a +2-3.5% correction, the log's +4%; ordinary jitter keeps it within a few tenths of a
// percent). Such a batch, its first output still waiting for its slot, takes the coupled decay instead of the rate
// unless its hand-over is clearly faster than our cadence (ordinary learning is then the faster way down), so the
// interval never grows while held. The target also follows its base down faster (kTargetFollow, see below).
// V1.6.2-alpha.24 heldrate.1 (a tester, Cyberpunk, RX 7900 XTX, beta.5 + XeFGFlipQueue = on, 10 native buffers, 6X, 23 Oct:
// 6.2 -> 9.4 ms per output, ~19 base fps against a ~23 ms production latency; #675 the same at 6X on the unmetered NVIDIA
// standalone queue): every first output was on time and only waited for our previous gate (production samples froze),
// and XeFG's presenter was backlogged on every batch, yet the game handed its batches over a few percent slower than our
// flips (one inner output per batch flipped late at readiness: gap p95 ~2.6 ms over p50), outside kFollowTolerance, so
// each was learned as the game's rate; only the phase correction (+1.5%) stopped the slide. On an unmetered queue
// (SetUnmetered: no driver flip metering times its batches) such a batch is held by our queue: coupled (backlogged), its
// first output on time and gate-bound (ready within kGateBoundSlack of our previous gate's signal). A hand-over slower
// than the rate is then not learned: the rate is kept (Kept()), neither slowed nor decayed (a decay could run the clock
// to its floor where a late inner output, not our queue, sets the batch). Faster hand-overs are still learned, and a
// game that really slows drains the queue, loses the backlog or the gate-bound first, and is learned again.
// V1.7.0 firstready.1 (XeMFG output below XeFGUnlock and V1.5.2 beta.2: a tester, The Ascent, RTX 3060 Ti, beta.5, 3X, menu
// closed 103-108 base at 85-91% GPU against ~114 at 98% while OptiScaler's menu kept every first output late; OW2, RX 6700 XT,
// beta.3, 46 base at 69% GPU against XeFGUnlock's 55.6 at 95%; build\xemfg-throughput-20261024): the buffer held each batch
// about one batch deep after the game handed it over (target = production + kProductionMargin), the game's own latency logic
// (XeLL's sleep, an engine's frame sync) slowed the game to match, and the rate, which only follows the hand-overs, learned
// that cadence. With SetFirstAtReadiness a batch's first output is not held for a buffer: it flips once ready, at least
// kFirstGap of an interval after the previous flip, and the phase correction is measured (for lockin.1's deep run) but
// not applied; the outputs inside a batch keep their spacing. Steady at the game's rate the first is then ready about one
// interval after the batch's real frame flipped, so nothing waits; a faster game shortens the gap and the rate learns the
// faster hand-overs. Closed-loop sims (devtests LatencyPacedGameReachesCapacity): a latency-paced game runs at the CPU
// pacer's rate (buffered: ~2 ms per batch slower). kFirstGap 0.9 rather than 0.5 holds an early first close to its slot,
// so a free game's jitter reaches the display at slope ~0.33 instead of ~0.9 (buffered ~0.5) for <1% of that rate.
// V1.7.0 bimodal.1 (a tester, Witcher 3, RTX 4070 Ti SUPER, beta.4, 3X, XeLL, metered: real batches ~12 ms, the clock 7.8-9 ms,
// 45-60 of every 60 first outputs late, flip gaps p50 ~2.5 ms / p95 ~11 ms, i.e. three outputs in ~5 ms and a long seam):
// XeLL's sleep made the hand-overs bimodal (short, then long), and hitch.1's rule left every long one out unless three came
// in a row, so the rate learned only the short ones. A long hand-over is now a hitch only when isolated: one within
// kHitchSpacing batches of the last long one is cadence and is averaged, at most kCadenceClamp times the rate per batch (a
// burst of real 50-250 ms hitches moves the rate 1/8 per hitch at most); three in a row still restart the rate.
// V1.7.0 deepheld.1 (ID9442, Wuthering Waves, RX 9070 XT, perffix.1, 3X-6X, unmetered: every batch took lockin.1's deep
// decay and the interval sat at kMinimumInterval, so the outputs flipped at readiness in bursts, flip gap p95 up to 11x
// the p50, while holds were 0 ms at p50): the game ran ahead of the GPU, so the previous batch's real frame was released
// 13-29 ms after the next hand-over at its own readiness, and lockin.1 read that slot latency as our queue holding the game.
// Since firstready.1 nothing holds a batch's first output, so a late slot no longer says the queue is behind. A deep decay
// (full, or the deep run) now also needs our clock to have held the previous batch's real frame, released kDeepHoldSlack or
// more after it was ready, as every output is in a real lock-in (lockin.1's log: 57.6 ms per output). A refused one is
// counted (DeepRefused, the GPU or the game's own queue behind, not ours). Whatever drives the interval to the floor, an
// interval pinned at kMinimumInterval for kFloorRun batches (1000 outputs a second: no display shows that) is a runaway:
// the rate restarts from the next hand-over and the reset is counted (FloorResets).
class JitterBufferClock
{
public:
    static constexpr int64_t kMinimumDelta = 1000000, kMaximumDelta = 250000000, kMaximumHold = 100000000;
    static constexpr double kFirstGap = 0.9;
    static constexpr double kRateGain = 0.125, kPhaseGain = 0.05, kMaximumCorrection = 0.04, kCoupledDecay = 32.0;
    static constexpr double kCoupledFloor = 4.0, kMinimumInterval = 1000000.0, kFollowTolerance = 0.02;
    static constexpr double kDeepTolerance = 0.1, kDeepCorrection = 0.025;
    static constexpr uint32_t kDeepRun = 3;
    static constexpr int64_t kProductionMargin = 2000000, kRelaxStep = 100000, kTargetFollow = 8;
    static constexpr size_t kProductionWindow = 64; // diagnostics only
    static constexpr int64_t kGateBoundSlack = 1500000;
    static constexpr int64_t kHitchNumerator = 3, kHitchDenominator = 2;
    static constexpr uint32_t kHitchRun = 3, kHitchSpacing = 8;
    static constexpr double kCadenceClamp = 2.0;
    static constexpr int64_t kDeepHoldSlack = 250000;
    static constexpr uint32_t kFloorRun = 60;
    // deepRefused (deepheld.1): this batch met lockin.1's deep conditions, but our clock had not held the previous real
    // frame; floorReset: the rate restarted after kFloorRun batches at kMinimumInterval.
    struct Release { int64_t ns{}, interval{}, latency{}, target{}; bool held{}, batchStart{}, late{}, hitch{}, coupled{}, deep{},
        deepRefused{}, floorReset{}; };

    void Reset() noexcept
    {
        const auto floor = floor_; const auto fraction = fraction_; const auto buffers = buffers_;
        const auto late = lateFirsts_; const auto hitches = hitches_; const auto coupled = coupled_; const auto deep = deep_;
        const auto kept = kept_; const auto unmetered = unmetered_;
        const auto firstReady = firstReady_; const auto gapHolds = gapHolds_;
        const auto deepRefused = deepRefused_; const auto floorResets = floorResets_;
        *this = {}; floor_ = floor; fraction_ = fraction; buffers_ = buffers; lateFirsts_ = late; hitches_ = hitches;
        coupled_ = coupled; deep_ = deep; kept_ = kept; unmetered_ = unmetered; firstReady_ = firstReady; gapHolds_ = gapHolds;
        deepRefused_ = deepRefused; floorResets_ = floorResets;
    }
    // Native swap chain back buffers (0 = unknown: the target may reach 1.5 batches).
    void SetCapacity(uint32_t buffers) noexcept { buffers_ = buffers; }
    // heldrate.1: no driver flip metering times this queue's batches (NVIDIA's metered batches keep the follow rule only).
    void SetUnmetered(bool unmetered) noexcept { unmetered_ = unmetered; }
    // firstready.1: a batch's first output flips at readiness (kFirstGap after the previous flip at the earliest), no buffer.
    void SetFirstAtReadiness(bool ready) noexcept { firstReady_ = ready; }
    bool FirstAtReadiness() const noexcept { return firstReady_; }
    // firstready.1: first outputs ready before kFirstGap after the previous flip, held to it.
    uint64_t GapHolds() const noexcept { return gapHolds_; }
    // Production latency (first output ready - batch hand-over) quantile; 0 until known.
    int64_t ProductionQuantile(double q) const noexcept
    {
        if (!productionCount_) return 0;
        std::array<int64_t, kProductionWindow> sorted{};
        for (size_t i = 0; i < productionCount_; ++i) sorted[i] = production_[(productionStart_+i)%kProductionWindow];
        const size_t k = size_t(q*double(productionCount_-1)+0.5);
        std::nth_element(sorted.begin(), sorted.begin()+k, sorted.begin()+productionCount_);
        return sorted[k];
    }
    int64_t Cap() const noexcept { return cap_; }
    void SetFloor(int64_t interval) noexcept { floor_ = interval > 0 && interval <= kMaximumHold ? interval : 0; }
    // Base buffer as a fraction of one batch period (0.5 = half a batch).
    void SetBufferFraction(double fraction) noexcept { fraction_ = fraction >= 0.0 && fraction <= 1.0 ? fraction : 0.5; }
    int64_t Interval() const noexcept { return effective_; }
    int64_t Target() const noexcept { return target_; }
    int64_t Latency() const noexcept { return latency_; }
    uint64_t LateFirsts() const noexcept { return lateFirsts_; }
    uint64_t Hitches() const noexcept { return hitches_; }
    uint64_t Coupled() const noexcept { return coupled_; }
    // lockin.1: coupled decays taken because the meter was proven behind (full, or a deep buffer; in Coupled()).
    uint64_t Deep() const noexcept { return deep_; }
    // heldrate.1: slower hand-overs not learned because an unmetered queue held the batch.
    uint64_t Kept() const noexcept { return kept_; }
    // deepheld.1: deep decays refused (our clock held nothing: the GPU or the game's queue was behind), and rate restarts
    // after the interval sat at the floor; LastHold: how long our clock held the last output after it was ready.
    uint64_t DeepRefused() const noexcept { return deepRefused_; }
    uint64_t FloorResets() const noexcept { return floorResets_; }
    int64_t LastHold() const noexcept { return lastHold_; }
    // Phase correction of the last batch in parts per million (positive = faster).
    int32_t CorrectionPpm() const noexcept { return int32_t(correction_*1e6); }

    // ready: this output's copy completion; submit: when the game handed this batch over (first outputs only);
    // outputs: generated outputs + the real frame; gateSignal: when the previous output's gate was signalled (0 =
    // unknown); coupled (first outputs): the game handed this batch over after being held by the full presentation
    // queue (a gated Present of the previous batch blocked in the native Present); full (lockin.1): that block waited
    // behind more than a whole batch of gates.
    Release Plan(int64_t ready, int64_t submit, bool first, uint32_t outputs, int64_t gateSignal = 0,
        bool coupled = false, bool full = false) noexcept
    {
        Release result{};
        if (ready <= 0 || outputs < 2) { Reset(); result.ns = ready; return result; }
        if (first)
        {
            result.batchStart = true;
            const int64_t delta = submit-lastSubmit_;
            // A live count change (Dynamic MFG) keeps the batch period: the per-output interval is rescaled, as
            // V1.6.0's estimator keeps its history, so the switch batch is not flipped as a burst.
            if (outputs != outputs_ && outputs_ && interval_ > 0.0) interval_ = interval_*double(outputs_)/double(outputs);
            // The first output is still waiting for its slot on the running clock (the queue held it).
            const bool onTime = released_ && effective_ > 0 && ready <= last_+effective_;
            // lockin.1: the last batches' buffers stayed half a batch or more above the target.
            deepRun_ = released_ && correction_ >= kDeepCorrection ? deepRun_+1 : 0;
            const double cadence = double(effective_)*double(outputs_);
            const bool follows = coupled && double(delta) <= cadence*(1.0+kFollowTolerance);
            const bool held = unmetered_ && coupled && onTime && gateSignal > 0 && ready <= gateSignal+kGateBoundSlack;
            const bool deepWanted = (full || deepRun_ >= kDeepRun) && double(delta) >= cadence*(1.0-kDeepTolerance);
            // deepheld.1: and our clock held the previous batch's real frame (the output planned before this one).
            const bool deep = deepWanted && lastHold_ >= kDeepHoldSlack;
            if (deepWanted && !deep && !follows && onTime && interval_ > 0.0) { result.deepRefused = true; ++deepRefused_; }
            if (submit <= 0 || !lastSubmit_ || delta < kMinimumDelta || delta > kMaximumDelta)
            { interval_ = 0.0; effective_ = 0; hitchRun_ = 0; deepRun_ = 0; }
            else if (onTime && interval_ > 0.0 && (follows || deep))
            {
                // The hand-over followed our own flips: probe faster instead of learning them, within bounds. A deep
                // queue's decay is bounded by its own conditions (its first output still on time, the buffer deep) instead
                // of the learned rate, which the lock-in itself inflated.
                const double floor = follows ? (std::max)(learned_/kCoupledFloor, kMinimumInterval) : kMinimumInterval;
                interval_ = (std::max)(interval_-interval_/kCoupledDecay, (std::min)(floor, interval_));
                result.coupled = true; ++coupled_; hitchRun_ = 0;
                if (!follows) { result.deep = true; ++deep_; }
            }
            else if (held && interval_ > 0.0 && double(delta) > interval_*double(outputs))
            { ++kept_; hitchRun_ = 0; } // heldrate.1: the rate stays
            else
            {
                const double batch = interval_*double(outputs);
                const bool hitch = interval_ > 0.0 && double(delta)*kHitchDenominator > batch*kHitchNumerator;
                // bimodal.1: only a long hand-over with none in the kHitchSpacing batches before it is left out.
                const bool isolated = sinceLong_ > kHitchSpacing;
                sinceLong_ = hitch ? 0 : (std::min)(sinceLong_+1, kHitchSpacing+1);
                if (hitch) ++hitchRun_;
                if (hitch && isolated && hitchRun_ < kHitchRun) { result.hitch = true; ++hitches_; }
                else
                {
                    // A lasting slowdown restarts the rate at the new cadence; a recurring long one is averaged.
                    const bool lasting = hitch && hitchRun_ >= kHitchRun;
                    double sample = double(delta)/double(outputs);
                    if (hitch && !lasting) sample = (std::min)(sample, interval_*kCadenceClamp);
                    interval_ = interval_ > 0.0 && !lasting ? interval_+kRateGain*(sample-interval_) : sample;
                    if (interval_ < kMinimumInterval) interval_ = kMinimumInterval;
                    learned_ = interval_;
                    if (!hitch || lasting) hitchRun_ = 0;
                }
            }
            // deepheld.1: an interval pinned at the floor restarts the rate (relearned from the next hand-over).
            floorRun_ = interval_ > 0.0 && interval_ <= kMinimumInterval*1.01 ? floorRun_+1 : 0;
            if (floorRun_ >= kFloorRun)
            {
                interval_ = 0.0; learned_ = 0.0; effective_ = 0; hitchRun_ = 0; deepRun_ = 0; floorRun_ = 0;
                result.floorReset = true; ++floorResets_;
            }
            lastSubmit_ = submit > 0 ? submit : 0; outputs_ = outputs;
            correction_ = 0.0;
            if (interval_ > 0.0)
            {
                const double batch = interval_*outputs;
                base_ = int64_t(fraction_*batch);
                cap_ = buffers_ ? int64_t(double(buffers_ > outputs+3 ? buffers_-outputs-2 : 1)*interval_) : int64_t(1.5*batch);
                if (base_ > cap_) base_ = cap_;
                if (!target_ || target_ < base_) target_ = base_;
                if (target_ > Ceiling()) target_ = Ceiling();
                // Buffer this batch would get if it flipped on the running clock.
                const int64_t slot = released_ ? last_+effective_ : 0;
                if (slot && submit > 0 && effective_ > 0)
                {
                    latency_ = slot-submit;
                    double error = double(latency_-target_)/batch;
                    error = error > 1.0 ? 1.0 : error < -1.0 ? -1.0 : error;
                    double correction = kPhaseGain*error;
                    correction = correction > kMaximumCorrection ? kMaximumCorrection
                        : correction < -kMaximumCorrection ? -kMaximumCorrection : correction;
                    correction_ = correction;
                    // firstready.1: the correction is still measured (lockin.1's deep run reads it) but not applied.
                    effective_ = firstReady_ ? int64_t(interval_) : int64_t(interval_*(1.0-correction));
                }
                else effective_ = int64_t(interval_);
                if (effective_ < floor_) effective_ = floor_;
            }
        }
        int64_t release = ready;
        // firstready.1: a first output waits only for the minimum gap after the previous flip (never under the V-Sync
        // floor), not for its slot.
        const int64_t spacing = first && firstReady_ ? (std::max)(int64_t(kFirstGap*double(effective_)), floor_) : effective_;
        if (released_ && effective_ > 0 && last_+spacing > ready) release = last_+spacing;
        if (release-ready > kMaximumHold) release = ready+kMaximumHold;
        result.held = release > ready;
        if (first && firstReady_ && result.held) ++gapHolds_;
        if (first && released_ && effective_ > 0 && ready > last_+effective_) { result.late = true; ++lateFirsts_; }
        if (first && target_)
        {
            const bool sample = !result.coupled && submit > 0 && ready > submit && ready-submit < kMaximumHold
                && (gateSignal <= 0 || ready > gateSignal+kGateBoundSlack);
            const int64_t production = sample ? ready-submit : 0;
            if (sample)
            {
                production_[(productionStart_+productionCount_)%kProductionWindow] = production;
                if (productionCount_ < kProductionWindow) ++productionCount_;
                else productionStart_ = (productionStart_+1)%kProductionWindow;
            }
            // Next batch's target (this batch already flipped on its own).
            if (sample && production+kProductionMargin > target_)
                target_ = production+kProductionMargin < Ceiling() ? production+kProductionMargin : Ceiling();
            else
            {
                // lockin.1: above what the production latency needs (a base set during a slow stretch: half of a
                // 140 ms batch stayed ~70 ms for hundreds of batches) the target follows 1/kTargetFollow of the excess.
                const int64_t need = (std::max)(base_, productionCount_ ? ProductionQuantile(0.9)+kProductionMargin : base_);
                const int64_t step = target_ > need ? (std::max)(kRelaxStep, (target_-need)/kTargetFollow) : kRelaxStep;
                target_ = target_-step > base_ ? target_-step : base_;
            }
        }
        released_ = true; last_ = release; lastHold_ = release-ready; // deepheld.1
        result.ns = release; result.interval = effective_; result.latency = latency_; result.target = target_;
        return result;
    }

private:
    // alpha.2: the highest target: the capacity is queue time after readiness, on top of the production latency (p90).
    int64_t Ceiling() const noexcept { return cap_+ProductionQuantile(0.9); }

    double interval_{}, fraction_{0.5}, correction_{}, learned_{};
    int64_t lastSubmit_{}, last_{}, effective_{}, target_{}, latency_{}, floor_{}, cap_{}, base_{};
    uint64_t lateFirsts_{}, hitches_{}, coupled_{}, deep_{}, kept_{}, gapHolds_{};
    std::array<int64_t, kProductionWindow> production_{};
    size_t productionStart_{}, productionCount_{};
    uint32_t outputs_{}, buffers_{}, hitchRun_{}, deepRun_{};
    // bimodal.1: first outputs since the last long hand-over (above kHitchSpacing: none recently).
    uint32_t sinceLong_{kHitchSpacing+1};
    // deepheld.1: the last output's hold (release - ready, >= 0), batches at the floor, and the two counters.
    int64_t lastHold_{};
    uint32_t floorRun_{};
    uint64_t deepRefused_{}, floorResets_{};
    bool released_{}, unmetered_{}, firstReady_{};
};

// V1.6.2 gatepass.1: the gate lead where nothing meters the flips (AMD, Intel; NVIDIA batches without flip metering).
// There each flip happens when the presentation queue gets past its gate, and the meter signals the gate this lead early.
// pacing.1 used a fixed 0.5 ms, from one GPU (RTX 4080: a Wait passes 0.5-0.8 ms after the CPU Signal, median 0.65 ms);
// AMD's queue-wake latency is unmeasured (build\amd-pacing-fsr41-20261021). The meter samples how long gates take to pass
// after their Signal; once kMinimumSamples are in, the lead is the median of the last kWindow, within the bounds.
class GatePassLead
{
public:
    static constexpr int64_t kMinimumLead = 200000, kMaximumLead = 2000000;
    static constexpr size_t kWindow = 64;
    static constexpr uint64_t kMinimumSamples = 32;

    // A gate's pass latency (pass - signal), > 0.
    void Add(int64_t passNs) noexcept
    {
        if (passNs <= 0) return;
        window_[(start_+count_)%kWindow] = passNs;
        if (count_ < kWindow) ++count_;
        else start_ = (start_+1)%kWindow;
        ++samples_;
        std::array<int64_t, kWindow> sorted{};
        for (size_t i = 0; i < count_; ++i) sorted[i] = window_[(start_+i)%kWindow];
        std::nth_element(sorted.begin(), sorted.begin()+count_/2, sorted.begin()+count_);
        median_ = sorted[count_/2];
    }
    uint64_t Samples() const noexcept { return samples_; }
    // Median of the window; 0 before the first sample.
    int64_t Median() const noexcept { return median_; }
    // fallback until kMinimumSamples are in.
    int64_t Lead(int64_t fallback) const noexcept
    {
        return samples_ < kMinimumSamples ? fallback : (std::clamp)(median_, kMinimumLead, kMaximumLead);
    }

private:
    std::array<int64_t, kWindow> window_{};
    size_t start_{}, count_{};
    uint64_t samples_{};
    int64_t median_{};
};
}
