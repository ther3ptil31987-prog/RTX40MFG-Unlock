// AmpereFlipObserverTests: the RTX 30 flip-metering path (ampere_flip_metering.cpp).
// No inputs: setting parser, one-time configuration, SetFlipConfig params reads,
// metered/unmetered classification and the Active() state.
#include "ampere_flip_metering.h"
#include <Windows.h>
#include <cstdio>
#include <iostream>
#include <string>
unsigned checks = 0, failures = 0;
void Check(bool ok, const char* text)
{ ++checks; failures += !ok; std::cout << (ok ? "PASS " : "FAIL ") << text << '\n'; }

using namespace ampere_flip_metering;

Setting WithValue(const std::wstring& ini, const wchar_t* value)
{
    DeleteFileW(ini.c_str());
    if (value) WritePrivateProfileStringW(kSection, kKey, value, ini.c_str());
    return ReadSetting(ini.c_str());
}

struct Params { uint32_t version, framesPerBatch, feedback, presented; float time; uint32_t minControl; };
static_assert(sizeof(Params) == kParamsBytes);

int main()
{
    // The ini goes beside this executable, not in %TEMP%: an executable under a
    // folder with a Low mandatory label runs at Low integrity and cannot create
    // files in the user's %TEMP%, but can in its own build folder.
    wchar_t self[MAX_PATH]{};
    const DWORD length = GetModuleFileNameW(nullptr, self, MAX_PATH);
    std::wstring ini(self, length);
    ini.resize(ini.find_last_of(L"\\/") + 1);
    ini += L"AmpereFlipObserverTests-" + std::to_wstring(GetCurrentProcessId()) + L".ini";
    if (!length || length >= MAX_PATH || !WritePrivateProfileStringW(kSection, kKey, L"1", ini.c_str()))
    {
        std::cout << "SETUP cannot write the test ini beside the executable (error " << GetLastError() << ")\n";
        return 2;
    }
    Check(WithValue(ini, nullptr) == Setting::eDefault, "missing file follows the default");
    Check(WithValue(ini, L"1") == Setting::eOn && WithValue(ini, L"TRUE") == Setting::eOn
        && WithValue(ini, L"on") == Setting::eOn, "1, true and on are on");
    Check(WithValue(ini, L"0") == Setting::eOff && WithValue(ini, L"false") == Setting::eOff
        && WithValue(ini, L"Off") == Setting::eOff, "0, false and off are off");
    Check(WithValue(ini, L"2") == Setting::eInvalid && WithValue(ini, L"yes please") == Setting::eInvalid,
        "anything else is invalid (follows the default)");
    Check(WithValue(ini, L"") == Setting::eDefault, "an empty value follows the default");
    DeleteFileW(ini.c_str());
    WritePrivateProfileStringW(L"Other", kKey, L"0", ini.c_str());
    Check(ReadSetting(ini.c_str()) == Setting::eDefault, "the key in another section is ignored");
    DeleteFileW(ini.c_str());
    Check(ReadSetting(nullptr) == Setting::eDefault && ReadSetting(L"") == Setting::eDefault, "no path follows the default");
    // Test builds: on unless turned off.
    Check(kOnByDefault && Enabled(Setting::eDefault) && Enabled(Setting::eInvalid) && Enabled(Setting::eOn)
        && !Enabled(Setting::eOff), "the flip-metering path is on by default; only 0/false/off turn it off");

    Params params{kParamsV2, 4, 0, 4, 33.3f, 0};
    uint32_t version = 0, frames = 0;
    Check(ReadParams(&params, version, frames) && version == kParamsV2 && frames == 4, "V2 params read");
    Check(!ReadParams(nullptr, version, frames) && !version && !frames, "null params refused");
    Check(!ReadParams(reinterpret_cast<const void*>(0x10), version, frames) && !version && !frames,
        "an unreadable params pointer is refused (SEH)");

    const uint8_t capsQuery[]{0x90, 0xB9, 0x9D, 0xB1, 0x94, 0x61, 0xC3};
    const uint8_t setFlipQuery[]{0xB9, 0x42, 0x8C, 0x14, 0xF3, 0xC3};
    Check(ContainsCapsQuery(capsQuery, sizeof(capsQuery)) && !ContainsCapsQuery(setFlipQuery, sizeof(setFlipQuery))
        && !ContainsCapsQuery(capsQuery, 4) && !ContainsCapsQuery(nullptr, 16), "caps-query generation marker");
    Check(!ImageQueriesCaps(nullptr), "no image, no caps query");
    // A loaded image: the System32 kernel32.dll has executable sections but no caps query.
    Check(!ImageQueriesCaps(GetModuleHandleW(L"kernel32.dll")), "a loaded image without the caps query");

    // Opt-in off: a metered call is recorded but never makes repeats admissible.
    testing::Reset();
    Configure(false);
    Configure(true);
    Check(Configured() && !OptIn(), "configuration latches its first value");
    Observe(kParamsV2, 4, 0);
    Check(!Active() && Read().metered == 1, "without the opt-in metering is recorded, not admitted");

    testing::Reset();
    Configure(true);
    Check(OptIn() && !Active() && !Observing(), "opt-in starts inactive and unobserved");
    MarkObserving();
    Check(Observing(), "observing once SetFlipConfig is forwarded");
    Check(!Observe(kParamsV2, 0, 0) && !Active(), "CPU-paced call (nFramesPerBatch 0) stays inactive");
    Check(Observe(kParamsV2, 4, 0) && Active(), "metered V2 call activates");
    Check(!Observe(kParamsV1, 2, 0) && Active(), "metered V1 fallback stays active");
    Check(Observe(kParamsV2, 4, -9) && !Active(), "a refused call deactivates");
    Check(Observe(kParamsV2, 6, 0) && Active(), "6 frames per batch is within capacity");
    Check(Observe(kParamsV2, 7, 0) && !Active(), "more frames per batch than capacity deactivates");
    Observe(kParamsV2, 3, 0);
    Check(Observe(0x30018u, 3, 0) && !Active(), "an unknown params version deactivates");
    Observe(kParamsV2, 3, 0);
    Check(Observe(0, 0, 0) && !Active(), "an unreadable params block deactivates");
    const auto counters = Read();
    Check(counters.calls == 10 && counters.metered == 5 && counters.unmetered == 3 && counters.ignored == 2,
        "calls are classified and counted");
    Check(counters.lastVersion == 0 && counters.lastFramesPerBatch == 0 && counters.transitions >= 8,
        "last call and transitions are recorded");
    std::cout << checks << " checks, " << failures << " failures\n";
    return failures ? 1 : 0;
}
