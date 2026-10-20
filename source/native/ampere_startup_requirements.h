#pragma once
#include <cstdint>

// RTX 30/20 answer to NGX's FrameGeneration feature-requirements query while
// Streamline starts. Streamline's plug-in manager asks it during slInit, before
// any device exists, and unloads sl.dlss_g when it reports FG unsupported on
// every adapter. Nothing later in the process can bring that plug-in back.
namespace ampere_startup_requirements
{
enum class Answer : uint32_t
{
    eNative,           // NGX's own answer is returned unchanged.
    eAdapterSupported, // NGX answered for the adapter: Supported, no minimum architecture.
    eReadyProvider,    // No usable FG in NGX, but the prepared provider and program are present.
    eEligibleOta,      // slInit only: no usable FG in NGX yet, but the pre-scanned OTA provider is eligible.
};

struct Query
{
    bool success = false;        // NVSDK_NGX_Result_Success
    bool adapterAnswer = false;  // FeatureSupported is AdapterUnsupported or Supported
    bool featureMissing = false; // NVSDK_NGX_Result_FAIL_FeatureNotFound or _FeatureNotSupported
    bool providerReady = false;  // the eligible provider and its full SM86/SM75 program
    bool slInit = false;         // asked inside slInit, where Streamline keeps or drops sl.dlss_g
    bool eligibleOta = false;    // the pre-slInit OTA scan selected an NVIDIA-signed, eligible provider
};

constexpr Answer Decide(const Query& query) noexcept
{
    if (query.success) return query.adapterAnswer ? Answer::eAdapterSupported : Answer::eNative;
    if (!query.featureMissing) return Answer::eNative;
    if (query.providerReady) return Answer::eReadyProvider;
    // NGX may not map any DLSS-G provider during slInit (no game copy, no app
    // OTA), and Streamline then unloads sl.dlss_g before the device exists.
    // The pre-scanned provider is mapped and verified at device setup, where
    // startup admission still fails closed. Do not invent a feature otherwise.
    return query.slInit && query.eligibleOta ? Answer::eEligibleOta : Answer::eNative;
}

constexpr const char* AnswerName(Answer answer) noexcept
{
    switch (answer)
    {
    case Answer::eNative: return "native";
    case Answer::eAdapterSupported: return "supported-for-adapter";
    case Answer::eReadyProvider: return "supported-prepared-provider";
    case Answer::eEligibleOta: return "supported-eligible-ota-provider";
    default: return "unknown";
    }
}
}
