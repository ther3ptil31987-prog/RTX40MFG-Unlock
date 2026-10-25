#pragma once
#include <cstddef>
#include <cstdint>

// RTX 30/20 answer to NGX's FrameGeneration feature-requirements query while
// Streamline starts. Streamline's plug-in manager asks it during slInit, before
// any device exists, and unloads sl.dlss_g when it reports FG unsupported on
// every adapter. Nothing later in the process can bring that plug-in back.
//
// InvalidParameter is the driver refusing the application identifier, not the
// GPU. Driver 617.42's GetFeatureRequirements (_nvngx.dll f039cd8a, RVA
// 0x452B0) checks a ProjectId identifier with EngineType CUSTOM before any GPU
// check, and fails it unless the project ID is a GUID (ProjectIdPassesDriverCheck).
// Streamline sends that identifier whenever the game sets a project ID or an
// engine version (FF7 Rebirth's StreamlineSubset). On any failure other than
// FeatureNotFound/NotSupported, Streamline keeps sl.dlss_g's default minimum
// architecture, Ada: RTX 40 keeps the plug-in and RTX 30/20 lose it.
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
    bool success = false;            // NVSDK_NGX_Result_Success
    bool adapterAnswer = false;      // FeatureSupported is AdapterUnsupported or Supported
    bool featureMissing = false;     // NVSDK_NGX_Result_FAIL_FeatureNotFound or _FeatureNotSupported
    bool identifierRejected = false; // NVSDK_NGX_Result_FAIL_InvalidParameter: the identifier check, never the GPU
    bool providerReady = false;      // the eligible provider and its full SM86/SM75 program
    bool slInit = false;             // asked inside slInit, where Streamline keeps or drops sl.dlss_g
    bool eligibleOta = false;        // the pre-slInit OTA scan selected an NVIDIA-signed, eligible provider
};

// NGX result codes (nvsdk_ngx_defs.h; ampere_backend.cpp checks them against the SDK).
constexpr uint32_t kResultSuccess = 0x1;
constexpr uint32_t kResultFeatureNotSupported = 0xBAD00001;
constexpr uint32_t kResultFeatureNotFound = 0xBAD00004;
constexpr uint32_t kResultInvalidParameter = 0xBAD00005;

// The result-code part of a query; every other failure code stays native.
constexpr void ClassifyResult(uint32_t result, Query& query) noexcept
{
    query.success = result == kResultSuccess;
    query.featureMissing = result == kResultFeatureNotSupported || result == kResultFeatureNotFound;
    query.identifierRejected = result == kResultInvalidParameter;
}

constexpr Answer Decide(const Query& query) noexcept
{
    if (query.success) return query.adapterAnswer ? Answer::eAdapterSupported : Answer::eNative;
    if (query.featureMissing && query.providerReady) return Answer::eReadyProvider;
    // NGX may not map any DLSS-G provider during slInit (no game copy, no app
    // OTA), or may refuse the identifier, and Streamline then unloads sl.dlss_g
    // before the device exists. The pre-scanned provider is mapped and verified
    // at device setup, where startup admission still fails closed. A refused
    // identifier never reaches the prepared-provider answer, and stays native
    // once that provider is prepared. Do not invent a feature otherwise.
    const bool answerable = query.featureMissing || (query.identifierRejected && !query.providerReady);
    return answerable && query.slInit && query.eligibleOta ? Answer::eEligibleOta : Answer::eNative;
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

// The driver's project ID check (617.42, RVA 0x452B0), for the log only: any
// number of {} [] () or "" wrappers are stripped, dashes are skipped, and the
// rest must be exactly 32 hex digits with no run of 6 identical characters
// (compared as written, so 'a' and 'A' differ; a dash does not end a run).
constexpr bool ProjectIdPassesDriverCheck(const char* id, size_t length) noexcept
{
    if (!id) return false;
    size_t begin = 0, end = length;
    while (end - begin > 1)
    {
        const char first = id[begin], last = id[end - 1];
        if (!((first == '{' && last == '}') || (first == '[' && last == ']')
                || (first == '(' && last == ')') || (first == '"' && last == '"')))
            break;
        ++begin;
        --end;
    }
    unsigned digits = 0, run = 0;
    char previous = '\0';
    for (size_t i = begin; i < end; ++i)
    {
        const char c = id[i];
        if (c == '-') continue;
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) return false;
        run = c == previous ? run + 1 : 0;
        previous = c;
        if (++digits > 32 || run > 4) return false;
    }
    return digits == 32;
}
}
