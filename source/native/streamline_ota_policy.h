#pragma once

#include <cstdint>

namespace streamline_ota_policy
{
constexpr uint64_t kAllowOta = 1ull << 3;
constexpr uint64_t kLoadDownloadedPlugins = 1ull << 6;
// Same values as gpu_dispatch::Family, checked at the integration boundary.
enum class Gpu : uint32_t { eUnknown, eAda, eAmpere, eConflict, eTuring, eBlackwell };
constexpr Gpu InventoryGpu(bool detected, uint32_t count, uint32_t architecture, bool ampereRtx) noexcept
{
    return !detected || count != 1 ? Gpu::eUnknown : ampereRtx ? (architecture == 0x160 ? Gpu::eTuring : Gpu::eAmpere)
        : architecture == 0x190 ? Gpu::eAda : Gpu::eUnknown;
}
enum class Reason : uint32_t
{ eNotListed, eNativePinned, eUnknownGpu, eConflict, eHost, eLoader,
  eSelectedAda, eSelectedAmpere, eEarlyAda, eEarlyAmpere, eSelectedTuring, eEarlyTuring, eSelectedBlackwell };
constexpr const char* ReasonName(Reason reason) noexcept
{
    switch (reason)
    {
    case Reason::eNotListed: return "not-listed";
    case Reason::eNativePinned: return "native-streamline-pinned";
    case Reason::eUnknownGpu: return "gpu-unknown";
    case Reason::eConflict: return "adapter-conflict";
    case Reason::eHost: return "host-incompatible";
    case Reason::eLoader: return "loader-not-ready";
    case Reason::eSelectedAda: return "selected-ada";
    case Reason::eSelectedAmpere: return "selected-ampere";
    case Reason::eEarlyAda: return "early-nvapi-ada";
    case Reason::eEarlyAmpere: return "early-nvapi-ampere";
    case Reason::eSelectedTuring: return "selected-turing";
    case Reason::eEarlyTuring: return "early-nvapi-turing";
    case Reason::eSelectedBlackwell: return "selected-blackwell";
    default: return "unknown";
    }
}

struct Result
{
    uint64_t flags = 0;
    bool allowOtaForced = false;
    bool loadDownloadedPluginsForced = false;
    bool fullOtaRequested = false;
    bool fullOtaSuppressed = false;
    Reason reason = Reason::eNotListed;
};

constexpr Result Apply(uint64_t flags, bool officialSixX,
    bool coherentHost, bool loaderDiscoveryReady, Gpu selected, Gpu early,
    bool nativePinned = false) noexcept
{
    const Gpu gpu = selected == Gpu::eUnknown ? early : selected;
    // RTX 50 (selected only, like RTX 40) gets the same coherent plug-in set
    // the NVIDIA app's override loads for an official 6X title.
    const bool supported = gpu == Gpu::eAda || gpu == Gpu::eAmpere || gpu == Gpu::eTuring || gpu == Gpu::eBlackwell;
    const bool enable = officialSixX && !nativePinned && coherentHost
        && loaderDiscoveryReady && supported;
    const Reason reason = !officialSixX ? Reason::eNotListed : nativePinned ? Reason::eNativePinned
        : gpu == Gpu::eConflict ? Reason::eConflict
        : !supported ? Reason::eUnknownGpu
        : !coherentHost ? Reason::eHost : !loaderDiscoveryReady ? Reason::eLoader
        : selected == Gpu::eAda ? Reason::eSelectedAda : selected == Gpu::eAmpere ? Reason::eSelectedAmpere
        : selected == Gpu::eTuring ? Reason::eSelectedTuring : selected == Gpu::eBlackwell ? Reason::eSelectedBlackwell
        : gpu == Gpu::eAda ? Reason::eEarlyAda : gpu == Gpu::eTuring ? Reason::eEarlyTuring : Reason::eEarlyAmpere;
    const uint64_t adjusted = enable
        ? flags | kAllowOta | kLoadDownloadedPlugins : flags;
    return {
        adjusted,
        enable && (flags & kAllowOta) == 0,
        enable && (flags & kLoadDownloadedPlugins) == 0,
        officialSixX,
        officialSixX && !enable,
        reason,
    };
}
}
