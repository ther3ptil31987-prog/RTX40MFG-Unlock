#include "ampere_startup_requirements.h"
#include "ampere_diagnostics.h"
#include <cstring>
#include <iostream>
unsigned checks = 0, failures = 0;
void Check(bool ok, const char* text)
{ ++checks; failures += !ok; std::cout << (ok ? "PASS " : "FAIL ") << text << '\n'; }
int main()
{
    using ampere_startup_requirements::Answer;
    using ampere_startup_requirements::Decide;
    using ampere_startup_requirements::Query;
    Query adapter{};
    adapter.success = true; adapter.adapterAnswer = true;
    Check(Decide(adapter) == Answer::eAdapterSupported, "NGX adapter answer becomes Supported without a minimum architecture");
    Query other{};
    other.success = true;
    Check(Decide(other) == Answer::eNative, "other successful answers (driver, OS) stay native");
    Query missing{};
    missing.featureMissing = true;
    Check(Decide(missing) == Answer::eNative, "FeatureNotFound without a prepared provider stays native");
    missing.providerReady = true;
    Check(Decide(missing) == Answer::eReadyProvider, "FeatureNotFound with the prepared provider becomes Supported");
    Query failed{};
    failed.providerReady = true;
    Check(Decide(failed) == Answer::eNative, "other NGX failures stay native even with a prepared provider");
    Query ota{};
    ota.featureMissing = true; ota.slInit = true; ota.eligibleOta = true;
    Check(Decide(ota) == Answer::eEligibleOta, "slInit FeatureNotFound with an eligible pre-scanned OTA provider becomes Supported");
    ota.eligibleOta = false;
    Check(Decide(ota) == Answer::eNative, "slInit FeatureNotFound without an eligible OTA provider stays native");
    ota.eligibleOta = true; ota.slInit = false;
    Check(Decide(ota) == Answer::eNative, "after slInit the OTA candidate alone never answers; the prepared provider does");
    ota.slInit = true; ota.providerReady = true;
    Check(Decide(ota) == Answer::eReadyProvider, "a prepared provider takes precedence over the OTA candidate");
    Query otaFailed{};
    otaFailed.slInit = true; otaFailed.eligibleOta = true;
    Check(Decide(otaFailed) == Answer::eNative, "other NGX failures stay native with an eligible OTA provider");
    Query otaOther{};
    otaOther.success = true; otaOther.slInit = true; otaOther.eligibleOta = true;
    Check(Decide(otaOther) == Answer::eNative, "a successful driver/OS refusal stays native with an eligible OTA provider");
    Check(std::strcmp(ampere_startup_requirements::AnswerName(Answer::eNative), "native") == 0
        && std::strcmp(ampere_startup_requirements::AnswerName(Answer::eReadyProvider), "supported-prepared-provider") == 0
        && std::strcmp(ampere_startup_requirements::AnswerName(Answer::eEligibleOta), "supported-eligible-ota-provider") == 0,
        "answers have stable log names");
    Check(ampere_diagnostics::kStartupWrapperMissingFailure == 177
        && std::strstr(ampere_diagnostics::FailureName(177), "DLSS-G plug-in at device setup") != nullptr
        && std::strcmp(ampere_diagnostics::FailureName(170), "early wrapper allocation could not be established") == 0,
        "a missing wrapper at device setup has its own failure, distinct from 170");
    std::cout << checks << " checks, " << failures << " failures\n";
    return failures ? 1 : 0;
}
