// DLSS-G 310.9.16 (Frame Generation Preset C) Ada admission: compiles the production midpoint_fix.cpp and
// checks the 26-name registration table, the unchanged midpoint transform and the OutputPull slot on
// data-mapped providers. No provider code runs.
#include "midpoint_fix.cpp"
#include "data_image.h"

int wmain(int argc, wchar_t** argv)
{
    using namespace midpoint_fix;
    namespace policy = dlssg_provider_policy;
    try
    {
        if (argc != 4) throw std::runtime_error("usage: audit 310.9.0.dll 310.9.16.dll output.fatbin");
        Require(ProfileForVersion({310, 9, 16}) == &k310916TemporalProfile, "310.9.16 binds the 26-name profile");
        Require(ProfileForVersion({310, 9, 0}) == &k3109TemporalProfile
            && ProfileForVersion({310, 9, 1}) == &k3109TemporalProfile, "310.9.0 and 310.9.1 keep the 310.9 profile");
        Require(!ProfileForVersion({310, 9, 15}) && !ProfileForVersion({310, 9, 17}) && !ProfileForVersion({310, 10, 0}),
            "neighbouring versions rejected");
        Require(policy::IsSupportedVersion({310, 9, 16}) && !policy::IsAdaOnlyVersion({310, 9, 16})
            && policy::IsSupportedAmpereVersion({310, 9, 16}), "310.9.16 eligible on Ada and the RTX 30 port");
        Require(policy::IsSupportedAmpereVersion({310, 9, 1}) && policy::ImplementsPresetC({310, 9, 16})
            && !policy::ImplementsPresetC({310, 9, 1}), "RTX 30 keeps 310.9.1; Preset C only from 310.9.16");

        policy::VersionTriplet oldVersion{}, newVersion{};
        Require(policy::ReadProviderVersion(argv[1], oldVersion) && oldVersion.major == 310 && oldVersion.minor == 9
            && oldVersion.build == 0, "first fixture is 310.9.0");
        Require(policy::ReadProviderVersion(argv[2], newVersion) && newVersion.major == 310 && newVersion.minor == 9
            && newVersion.build == 16, "second fixture is 310.9.16");
        DataImage oldImage(argv[1]);
        DataImage newImage(argv[2]);
        const auto oldModule = reinterpret_cast<HMODULE>(oldImage.base);
        const auto newModule = reinterpret_cast<HMODULE>(newImage.base);
        uint32_t oldBytes = 0, newBytes = 0;
        Require(ImageSize(oldModule, oldBytes) && oldBytes == oldImage.bytes
            && ImageSize(newModule, newBytes) && newBytes == newImage.bytes, "source PE image bounds");

        Require(FindDescriptorEntry(oldModule, oldBytes, k3109TemporalProfile) != 0, "310.9 profile still finds the 310.9.0 table");
        Require(FindDescriptorEntry(oldModule, oldBytes, k310916TemporalProfile) == 0, "26-name profile rejects the 310.9.0 table");
        Require(FindDescriptorEntry(newModule, newBytes, k3109TemporalProfile) == 0, "25-name profile rejects the 310.9.16 table");

        const auto& profile = k310916TemporalProfile;
        const uintptr_t begin = reinterpret_cast<uintptr_t>(newImage.base);
        const uintptr_t entry = FindDescriptorEntry(newModule, newBytes, profile);
        Require(entry != 0, "unique complete named 26-slot table discovered");
        auto* table = reinterpret_cast<uintptr_t*>(entry - profile.temporalSlot * sizeof(uintptr_t));
        auto* descriptor = reinterpret_cast<uint8_t*>(table[profile.temporalSlot]);
        const auto* source = reinterpret_cast<const uint8_t*>(ReadU64(descriptor + 8));
        std::cout << std::hex << "table_rva=0x" << reinterpret_cast<uintptr_t>(table) - begin
            << " slot10_entry_rva=0x" << entry - begin
            << " slot10_descriptor_rva=0x" << reinterpret_cast<uintptr_t>(descriptor) - begin
            << " slot10_fatbin_rva=0x" << reinterpret_cast<uintptr_t>(source) - begin << std::dec << '\n';
        Require(Sha256Equals(source, static_cast<size_t>(profile.sourceFatbinBytes), profile.sourceFatbinSha256),
            "midpoint source fatbin identical to 310.9");
        std::vector<uint8_t> fatbin(kOutputCapacity);
        std::vector<uint8_t> scratch(kScratchCapacity);
        std::memcpy(fatbin.data(), source, static_cast<size_t>(profile.sourceFatbinBytes));
        uint32_t outputBytes = 0;
        Failure failure{};
        Require(BuildTemporalFatbin(fatbin.data(), scratch.data(), profile, outputBytes, failure),
            "unchanged production temporal transformation");
        Require(Sha256Equals(fatbin.data(), outputBytes, profile.outputFatbinSha256), "exact 310.9 output fatbin SHA256");
        std::ofstream output(std::filesystem::path(argv[3]), std::ios::binary);
        output.write(reinterpret_cast<const char*>(fatbin.data()), outputBytes);
        Require(output.good(), "transformed artifact written");
        output.close();

        // OutputPull mask: slot 15 carries the exact 310.9 OutputPull payload.
        const uintptr_t end = begin + newBytes;
        Require(DescriptorMatches(begin, end, table[profile.outputPullSlot], profile.outputPullSlot, profile),
            "OutputPull descriptor at slot 15");
        auto* pull = reinterpret_cast<const uint8_t*>(table[profile.outputPullSlot]);
        Require(ReadU64(pull + 32) == 256 && ReadU64(pull + 40) == 1, "OutputPull launch fields the mask requires");
        const auto* mask = MaskProfileForTemporal(&profile);
        Require(mask == &kOutputPullMask3109, "310.9.16 uses the 310.9 OutputPull mask profile");
        const auto* pullSource = reinterpret_cast<const uint8_t*>(ReadU64(pull + 8));
        Require(Sha256Equals(pullSource, mask->sourceBytes, mask->sourceSha256), "OutputPull source payload identical to 310.9");
        Require(!DescriptorMatches(begin, end, table[14], profile.outputPullSlot, profile),
            "the old fixed slot 14 (now MenuDetectionOutput) is not accepted as OutputPull");

        // Negative cases on the 310.9.16 layout.
        std::swap(table[9], table[10]);
        Require(FindDescriptorEntry(newModule, newBytes, profile) == 0, "swapped kernel slots rejected");
        std::swap(table[9], table[10]);
        const uintptr_t fgLite = table[1];
        table[1] = table[0];
        Require(FindDescriptorEntry(newModule, newBytes, profile) == 0, "missing BlendCandidatesFGLite rejected");
        table[1] = fgLite;
        auto* sourceMutable = const_cast<uint8_t*>(source);
        sourceMutable[200] ^= 1;
        Require(FindDescriptorEntry(newModule, newBytes, profile) == 0, "mutated midpoint fatbin rejected");
        sourceMutable[200] ^= 1;
        Require(FindDescriptorEntry(newModule, newBytes, profile) == entry, "original mapping restored");
        std::cout << "ADA_310916_AUDIT_PASSED\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << "AUDIT_FAILED: " << error.what() << '\n';
        return 1;
    }
}
