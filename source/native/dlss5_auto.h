#pragma once

#include "dlss5_policy.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>

// DLSS 5 Auto (V1.7.0): placement, resolution scale and exposure chosen for
// the game at hand from what the stage observes, then adapted while it runs.
// Pure policy; dlss5.cpp supplies the measurements (GPU time of the stage,
// log-average luminance of its color, the real frame interval).
namespace dlss5::auto_mode
{
// Hook Method Auto: after upscaling, where the provider sees the reconstructed,
// temporally stable image, with Resolution Scale managing the cost. Before
// upscaling processes jittered render-resolution input and is never chosen.
// Present runs on the NGX instance of the game's DLSS evaluations too, so it
// offers nothing to a game without DLSS; it stays a choice of its own.
constexpr Stage AutoStage() noexcept { return Stage::eUpscaled; }

// The Resolution Scale levels Auto moves between, highest first.
inline constexpr std::array<float, 6> kLevels{100.0f, 85.0f, 75.0f, 66.0f, 50.0f, 33.0f};

// Auto's quality floor (autoscale.1): it never goes below 75 %. In #692's
// provider A/B at 1080p, 75 % kept 61.5 % of DLSS 5's change and 22.9 % of its
// fine detail, 50 % only 41.1 % and 8.5 %. When the floor is
// still over budget Auto stays there: the frame rate pays, not the detail.
inline constexpr size_t kFloorLevel = 2;
static_assert(kLevels[kFloorLevel] == 75.0f);

// The starting level before anything is measured: native up to 1440p-class
// extents, the floor (75 %) beyond.
inline size_t StartingLevel(uint32_t width, uint32_t height) noexcept
{
    const uint64_t pixels = uint64_t(width) * height;
    return pixels <= 2560ull * 1600 ? 0 : kFloorLevel;
}

// Auto never goes above the user's Resolution Scale: the highest level at or
// below it (the lowest level when the slider is below every level).
inline size_t CeilingLevel(float userScale) noexcept
{
    for (size_t level = 0; level < kLevels.size(); ++level)
        if (kLevels[level] <= userScale + 0.01f) return level;
    return kLevels.size() - 1;
}

// The lowest level Auto goes to: the quality floor, or the slider's own level
// when the slider is below the floor (the user's choice wins).
inline size_t FloorLevel(size_t ceiling) noexcept
{
    return std::max(kFloorLevel, std::min(ceiling, kLevels.size() - 1));
}

// The stage's GPU budget: DLSS 5 may take kBudgetShare of each real (rendered)
// frame; NR runs once per rendered frame, generated frames do not run it. The
// frame interval includes the stage itself, so the budget comes from the rest
// of the frame (the interval minus the stage's own GPU time, stageMs). That
// rest does not move with the Resolution Scale, so a lower scale (a faster
// stage, a higher frame rate) no longer shrinks the budget it is held to, as
// 30 % of the whole interval did. Without a stage time the interval is taken
// as it is (kBudgetShare of it).
inline constexpr float kBudgetShare = 0.30f, kMinBudgetMs = 2.0f, kMaxBudgetMs = 12.0f;

inline float BudgetMs(float realFrameMs, float stageMs = -1.0f) noexcept
{
    if (!std::isfinite(realFrameMs) || realFrameMs <= 0.0f) return kMaxBudgetMs;
    const float rest = std::isfinite(stageMs) && stageMs >= 0.0f ? std::max(realFrameMs - stageMs, 0.0f)
        : realFrameMs * (1.0f - kBudgetShare);
    return std::clamp(rest * kBudgetShare / (1.0f - kBudgetShare), kMinBudgetMs, kMaxBudgetMs);
}

// Adaptive Resolution Scale. A level change creates another feature, and the
// new one starts without temporal history, so changes are rare: down after a
// sustained overload, never below FloorLevel, up only when the next level's
// predicted cost fits comfortably, and never within kMinStepSeconds of the
// previous change.
struct ScaleController
{
    static constexpr double kMinStepSeconds = 8.0, kDownAfterSeconds = 1.5, kUpAfterSeconds = 6.0;
    static constexpr float kDownAbove = 1.15f, kUpBelow = 0.85f, kSmoothing = 0.1f;
    static constexpr uint32_t kMinSamples = 10;

    size_t level = 0;
    float averageMs = 0.0f;
    uint32_t samples = 0;
    double sinceStep = 0.0, overload = 0.0, headroom = 0.0;
    // At the quality floor (below the slider's level) and over budget for
    // kDownAfterSeconds: the menu says the frame rate pays (FloorHeld).
    bool floorHeld = false;

    void Reset(size_t start) noexcept
    {
        *this = {};
        level = std::min(start, kLevels.size() - 1);
    }

    float Scale() const noexcept { return kLevels[level]; }

    // The level within [ceiling, FloorLevel(ceiling)]. True when it moved.
    bool Clamp(size_t ceiling) noexcept
    {
        ceiling = std::min(ceiling, kLevels.size() - 1);
        const size_t lowest = FloorLevel(ceiling);
        if (level < ceiling) { Step(ceiling); return true; }
        if (level > lowest) { Step(lowest); return true; }
        return false;
    }

    // One completed measurement of the stage at the current level. ceiling:
    // CeilingLevel of the user's slider. True when `level` changed.
    bool Update(float gpuMs, float budgetMs, double dtSeconds, size_t ceiling) noexcept
    {
        if (Clamp(ceiling)) return true;
        ceiling = std::min(ceiling, kLevels.size() - 1);
        if (!std::isfinite(gpuMs) || gpuMs < 0.0f || !std::isfinite(dtSeconds) || dtSeconds < 0.0) return false;
        dtSeconds = std::min(dtSeconds, 1.0);
        averageMs = samples ? averageMs + kSmoothing * (gpuMs - averageMs) : gpuMs;
        ++samples;
        sinceStep += dtSeconds;
        bool fitsHigher = false;
        if (level > ceiling)
        {
            const float ratio = kLevels[level - 1] / kLevels[level];
            fitsHigher = averageMs * ratio * ratio < budgetMs * kUpBelow;
        }
        if (averageMs > budgetMs * kDownAbove) { overload += dtSeconds; headroom = 0.0; }
        else if (fitsHigher) { headroom += dtSeconds; overload = 0.0; }
        else { overload = 0.0; headroom = 0.0; }
        const bool atFloor = level >= FloorLevel(ceiling);
        floorHeld = atFloor && level > ceiling && overload >= kDownAfterSeconds && samples >= kMinSamples;
        if (sinceStep < kMinStepSeconds || samples < kMinSamples) return false;
        if (overload >= kDownAfterSeconds && !atFloor) { Step(level + 1); return true; }
        if (headroom >= kUpAfterSeconds && level > ceiling) { Step(level - 1); return true; }
        return false;
    }

private:
    void Step(size_t next) noexcept
    {
        level = next;
        averageMs = 0.0f;
        samples = 0;
        sinceStep = overload = headroom = 0.0;
        floorHeld = false;
    }
};

// Adaptive exposure: scales the stage color the network sees so its
// log-average luminance sits at middle grey, smoothed over time. The decode
// pass divides by the same factor, so the game's brightness is unchanged.
// Only for unbounded (HDR) color; display-referred SDR color is already exposed.
struct ExposureController
{
    static constexpr float kTarget = 0.18f, kMinGain = 0.125f, kMaxGain = 8.0f;
    static constexpr double kSeconds = 0.8;

    float logGain = 0.0f;
    bool valid = false;

    void Reset() noexcept { *this = {}; }

    float Gain() const noexcept { return std::exp2(logGain); }

    // logAverage: mean log2 luminance of the exposed stage color measured
    // without this controller's gain; aim: the luminance it is brought to
    // (Adaptive exposure target, dlss5adv.1).
    void Update(float logAverage, double dtSeconds, float aim = kTarget) noexcept
    {
        if (!std::isfinite(logAverage) || !std::isfinite(dtSeconds) || dtSeconds < 0.0) return;
        if (!(aim > 0.0f) || !std::isfinite(aim)) aim = kTarget;
        const float target = std::clamp(std::log2(aim) - logAverage,
            std::log2(kMinGain), std::log2(kMaxGain));
        if (!valid) { logGain = target; valid = true; return; }
        const double blend = 1.0 - std::exp(-std::min(dtSeconds, 1.0) / kSeconds);
        logGain += static_cast<float>(blend) * (target - logGain);
    }
};

// The real frame interval from successive stage evaluations, smoothed.
struct FrameClock
{
    float averageMs = 0.0f;
    uint64_t last = 0;

    // now and frequency: QueryPerformanceCounter units. Returns the interval
    // since the previous call in seconds (0 for the first call).
    double Tick(uint64_t now, uint64_t frequency) noexcept
    {
        if (!frequency || !last || now <= last) { last = now; return 0.0; }
        const double seconds = double(now - last) / double(frequency);
        last = now;
        // A pause (menu, loading, alt-tab) is not a frame interval.
        if (seconds > 0.25) return 0.0;
        const float ms = static_cast<float>(seconds * 1000.0);
        averageMs = averageMs > 0.0f ? averageMs + 0.05f * (ms - averageMs) : ms;
        return seconds;
    }
};
}
