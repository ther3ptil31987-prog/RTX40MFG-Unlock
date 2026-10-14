#pragma once

#include <cstdint>
#include "ampere_wrapper_capacity.h"

namespace ampere_policy
{
constexpr uint32_t kPresetSetting = 0x10E41DF1u;
constexpr uint32_t kPresetB = 2;
constexpr uint32_t kMaximumGeneratedFrames = ampere_mfg::kMaximumGeneratedFrames;

constexpr bool CapacityValid(uint32_t generated) noexcept
{
    return generated == 1 || generated == kMaximumGeneratedFrames;
}

constexpr bool EvaluationCountValid(int count) noexcept
{
    return count >= 1 && count <= static_cast<int>(kMaximumGeneratedFrames);
}

// One native render interval owns a fixed count of distinct submissions.
// Counts may change only between complete intervals, within the same proven
// allocation. A failed interval can restart only at index 1 of a newer frame.
// This records successful submissions, not completed GPU work or presentation.
struct Batch
{
    uint64_t frame = 0;
    std::array<uintptr_t, kMaximumGeneratedFrames> outputs{};
    uint32_t generated = 0;
    uint32_t submitted = 0;
    bool started = false;
    bool failed = false;

    bool Accepts(uint64_t value, int count, int index, uintptr_t output) const noexcept
    {
        if (!output || !EvaluationCountValid(count) || index < 1 || index > count) return false;
        if (!started) return index == 1;
        if (failed || submitted == generated)
            return index == 1 && value > frame;
        if (value != frame || count != generated || index != submitted + 1) return false;
        for (uint32_t i = 0; i < submitted; ++i)
            if (output == outputs[i]) return false;
        return true;
    }
    // The next call of the current interval, refused only because its output is
    // one this interval already submitted (a wrapper sharing one surface across
    // the batch, e.g. flip-metering presentation's two-surface pool).
    bool DuplicateOutput(uint64_t value, int count, int index, uintptr_t output) const noexcept
    {
        if (!started || failed || !output || submitted >= generated || value != frame
            || count != static_cast<int>(generated) || index != static_cast<int>(submitted) + 1) return false;
        for (uint32_t i = 0; i < submitted; ++i)
            if (output == outputs[i]) return true;
        return false;
    }
    void Fail(uint64_t value, int count, int index) noexcept
    {
        // Retain the failed interval even when its first native call failed.
        // A retry of that frame, a stale frame, or its remaining indices must
        // never be forwarded. No feature/module/publication proof is reset.
        if ((!started || (index == 1 && value > frame)) && EvaluationCountValid(count))
        {
            frame = value; generated = static_cast<uint32_t>(count);
            outputs = {}; submitted = 0; started = true;
        }
        failed = true;
    }
    void Complete(uint64_t value, int count, int index, uintptr_t output, bool succeeded) noexcept
    {
        if (!succeeded || !Accepts(value, count, index, output)) { Fail(value, count, index); return; }
        if (index == 1) { frame = value; generated = static_cast<uint32_t>(count); outputs = {}; started = true; failed = false; }
        outputs[static_cast<size_t>(index - 1)] = output;
        submitted = static_cast<uint32_t>(index);
    }
};

constexpr bool AdapterMatches(uint64_t d3dLuid, uint64_t cudaLuid,
    unsigned matches, int major, int minor, unsigned nodeMask) noexcept
{
    return d3dLuid != 0 && d3dLuid == cudaLuid && matches == 1
        && major == 8 && minor == 6 && nodeMask != 0
        && (nodeMask & (nodeMask - 1)) == 0;
}

constexpr bool ControlValid(unsigned multiplier, bool dynamic,
    bool experimental, bool generatedOnly) noexcept
{
    return multiplier >= 1 && multiplier <= kMaximumGeneratedFrames + 1
        && (!dynamic || multiplier != 1) && !experimental && !generatedOnly;
}

// The DLSS-G options RTXMFG submits on RTX 30. Follow game keeps the game's
// own mode, frame count and Dynamic target; the count is limited to the safe
// maximum, and Dynamic needs the full-capacity Dynamic proof (otherwise On).
// A fixed or Dynamic menu choice replaces the game's request.
enum class OptionsMode : uint8_t { eOff, eOn, eDynamic };

struct OptionsDecision
{
    OptionsMode mode = OptionsMode::eOff;
    uint32_t generatedFrames = 1;
    bool gameDynamicTarget = false;
};

constexpr OptionsDecision DecideOptions(bool followGame, uint32_t controlMultiplier,
    bool controlDynamic, OptionsMode gameMode, uint32_t gameGeneratedFrames,
    uint32_t safeMaximumMultiplier, bool dynamicSupported) noexcept
{
    const uint32_t limit = safeMaximumMultiplier > 2 ? safeMaximumMultiplier - 1 : 1;
    OptionsDecision decision{};
    if (followGame)
    {
        decision.generatedFrames = gameGeneratedFrames < 1 ? 1
            : gameGeneratedFrames > limit ? limit : gameGeneratedFrames;
        decision.gameDynamicTarget = gameMode == OptionsMode::eDynamic && dynamicSupported;
        decision.mode = gameMode == OptionsMode::eOff ? OptionsMode::eOff
            : decision.gameDynamicTarget ? OptionsMode::eDynamic : OptionsMode::eOn;
        return decision;
    }
    const uint32_t multiplier = controlMultiplier < 1 ? 1
        : controlMultiplier > safeMaximumMultiplier ? safeMaximumMultiplier : controlMultiplier;
    decision.generatedFrames = multiplier > 2 ? multiplier - 1 : 1;
    decision.mode = controlMultiplier == 1 || gameMode == OptionsMode::eOff ? OptionsMode::eOff
        : controlDynamic && dynamicSupported ? OptionsMode::eDynamic : OptionsMode::eOn;
    return decision;
}

// The DLSS-G maximum the game sees (numFramesToGenerateMax). A full-capacity
// wrapper offers its five generated frames only up to NVIDIA's per-title
// multiplier ceiling, so a 4X title is not offered 5x/6x that the options
// path would then clamp. A legacy 2x wrapper always reports one frame.
constexpr uint32_t AdvertisedGeneratedFrames(uint32_t startupMaximum,
    uint32_t policyMaximumMultiplier) noexcept
{
    if (startupMaximum != kMaximumGeneratedFrames) return 1;
    const uint32_t limit = policyMaximumMultiplier > 2 ? policyMaximumMultiplier - 1 : 1;
    return limit < kMaximumGeneratedFrames ? limit : kMaximumGeneratedFrames;
}

// Public NGX handles are opaque. No EndpointCore offsets, inferred allocation
// size, or unobserved release are accepted as a substitute for this identity.
struct Feature
{
    uintptr_t handle = 0;
    uintptr_t owner = 0;
    uint64_t generation = 0;
    uint64_t publication = 0;
    uint64_t luid = 0;
    bool evaluated = false;
    uintptr_t provider = 0;
    uint64_t providerGeneration = 0;
    uintptr_t providerHandle = 0;
    uint32_t createdGeneratedFrames = 0;
    uintptr_t device = 0; // Identity only; NGX owns the device lifetime.
    uint64_t outputWidth = 0;
    uint32_t outputHeight = 0, outputFormat = 0;
    Batch batch{};
    uintptr_t wrapper = 0;
    uint64_t wrapperGeneration = 0;
    ampere_wrapper::Capacity capacity{};
    // Distinguishes successive native feature lifetimes when NGX reuses the
    // same opaque handle and all module/publication identities are unchanged.
    // Kept after the original aggregate fields so policy fixtures remain exact.
    uint64_t lifetime = 0;
    uint64_t createAttemptToken = 0;
    uint64_t evaluationSerial = 0;
    bool rejectionReported = false;
    bool submissionFailureReported = false;
    bool recoveryPending = false;
    // The wrapper shared one output across a batch: this feature stays at 2x
    // (certified one generated frame) until it is released and recreated.
    bool outputReuseLatched = false;

    bool Matches(uintptr_t value, uintptr_t module, uint64_t load,
        uint64_t program, uint64_t adapter) const noexcept
    {
        return handle && value == handle && owner == module && generation == load
            && publication == program && luid == adapter;
    }
    bool Release(bool succeeded) noexcept
    {
        if (!handle || !succeeded) return false;
        *this = {};
        return true;
    }
};
}
