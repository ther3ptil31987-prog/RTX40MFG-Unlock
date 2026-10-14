// AmpereFlipMeteringTests <sl.dlss_g wrapper> <present|absent>
// The wrapper file is read as data, never loaded. "present": its flip-metering
// probe asks nvapi_QueryInterface for exactly the two interfaces the RTX 30
// filter withholds, once each, and nothing else the filter withholds.
#include "ampere_flip_metering.h"
#include <cstring>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <vector>
unsigned checks = 0, failures = 0;
void Check(bool ok, const char* text)
{ ++checks; failures += !ok; std::cout << (ok ? "PASS " : "FAIL ") << text << '\n'; }

size_t Count(const std::vector<unsigned char>& data, const std::vector<unsigned char>& pattern)
{
    size_t found = 0;
    for (size_t i = 0; i + pattern.size() <= data.size(); ++i)
        found += memcmp(data.data() + i, pattern.data(), pattern.size()) == 0;
    return found;
}
std::vector<unsigned char> MovEcx(uint32_t id)
{
    return {0xB9, static_cast<unsigned char>(id), static_cast<unsigned char>(id >> 8),
        static_cast<unsigned char>(id >> 16), static_cast<unsigned char>(id >> 24)};
}
std::vector<unsigned char> Text(const char* text) { return {text, text + strlen(text)}; }

int main(int argc, char** argv)
{
    if (argc != 3) { std::cerr << "usage: AmpereFlipMeteringTests <wrapper> <present|absent>\n"; return 2; }
    std::ifstream file(argv[1], std::ios::binary);
    const std::vector<unsigned char> data{std::istreambuf_iterator<char>(file), {}};
    Check(data.size() > 4096 && data[0] == 'M' && data[1] == 'Z', "wrapper image read as data");
    const bool present = std::string(argv[2]) == "present";
    using namespace ampere_flip_metering;
    const size_t setFlip = Count(data, MovEcx(kSetFlipConfig)), caps = Count(data, MovEcx(kFlipConfigCaps));
    const size_t probe = Count(data, Text("pfnNvAPI_SetFlipConfig API available"));
    if (present)
    {
        Check(probe == 1, "flipMetering initialize probe is present");
        Check(setFlip == 1, "SetFlipConfig is queried once, by that probe");
        // The V2 capability query arrived after 2.7; older probes ask for SetFlipConfig only.
        Check(caps <= 1, "the V2 capability query, where present, is queried once");
        Check(Count(data, Text("Achieved 'good' FC feedback state")) == 1,
            "the 30-feedback switch into flip-metering presentation is present");
        std::cout << "INFO capsQuery=" << caps << " cloneOutputPoolName="
            << Count(data, Text("clone.dlfg-output")) << '\n';
    }
    else
    {
        Check(probe == 0 && setFlip == 0 && caps == 0, "this wrapper has no flip-metering probe to withhold");
    }
    Check(Hidden(kSetFlipConfig) && Hidden(kFlipConfigCaps) && !Hidden(0x0150E828u),
        "only the two flip-metering interfaces are withheld");
    std::cout << checks << " checks, " << failures << " failures\n";
    return failures ? 1 : 0;
}
