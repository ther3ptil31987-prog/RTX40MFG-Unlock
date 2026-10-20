#pragma once
#include "gpu_dispatch.h"

namespace gpu_dispatch
{
// A successful selection is immutable for the process lifetime. A later
// contradictory device cannot replace a temporal program already in use.
struct Selection
{
    Family family = Family::eUnknown;
    uint64_t luid = 0;

    bool Allows(uint64_t candidate) const noexcept
    {
        return candidate && family != Family::eConflict
            && (family == Family::eUnknown || luid == candidate);
    }
    bool Observe(Family candidate, uint64_t identity) noexcept
    {
        if (!Allows(identity))
        {
            if (family != Family::eUnknown) family = Family::eConflict;
            return false;
        }
        const bool supported = candidate == Family::eAda || candidate == Family::eAmpere || candidate == Family::eTuring
            || candidate == Family::eBlackwell;
        if (!supported || (family != Family::eUnknown && family != candidate))
        {
            if (family != Family::eUnknown) family = Family::eConflict;
            return false;
        }
        family = candidate;
        luid = identity;
        return true;
    }
};

// RTX 30 Vulkan calls are owned by the Ampere backend's Vulkan route, which
// applies its own create, capacity and batch admission before these entries.
// RTX 50 frame generation is native and unpatched, so like an unknown GPU it
// is never blocked here; its menu controls still need the verified adapter.
inline bool AllowVulkanCreate(Family family, bool frameGeneration, bool adapterVerified) noexcept
{
    if (!frameGeneration) return true;
    return family == Family::eUnknown || family == Family::eBlackwell
        || ((family == Family::eAda || family == Family::eAmpere || family == Family::eTuring) && adapterVerified);
}

inline bool AllowVulkanEvaluate(Family family, bool provider, bool adapterVerified) noexcept
{
    if (family == Family::eConflict) return false;
    // A shared runtime can also evaluate SR/RR. The separately identified
    // DLSS-G provider is the authoritative place to block unsupported FG.
    return !provider || family == Family::eUnknown || family == Family::eBlackwell
        || ((family == Family::eAda || family == Family::eAmpere || family == Family::eTuring) && adapterVerified);
}
}
