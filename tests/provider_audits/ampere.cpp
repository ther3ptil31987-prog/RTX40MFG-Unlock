// RTX 30 port of DLSS-G 310.9.16: compiles the production ampere_gpu.cpp with the embedded SM86 caches in AUTO
// mode. Both providers are mapped as images without running their code (DONT_RESOLVE_DLL_REFERENCES). Checks the
// registration-bound 26-slot table, transforms every slot as PatchProvider does, compares the unchanged midpoint
// and OutputPull programs with 310.9.0's, and requires the complete 26-slot program to select the native cache.
#include "ampere_gpu.cpp"
#include "data_image.h"

namespace
{
using namespace ampere_gpu;

struct Provider
{
    HMODULE module = nullptr;
    uint32_t bytes = 0;
    const TemporalProviderProfile* profile = nullptr;
    uintptr_t table = 0;
    size_t slots = 0;
};

Provider Map(const wchar_t* path, uint32_t build)
{
    Provider provider;
    dlssg_provider_policy::VersionTriplet version{};
    Require(dlssg_provider_policy::ReadProviderVersion(path, version) && version.major == 310 && version.minor == 9
        && version.build == build, "provider fixture version");
    provider.module = LoadLibraryExW(path, nullptr, DONT_RESOLVE_DLL_REFERENCES);
    Require(provider.module && ImageSize(provider.module, provider.bytes), "provider mapped as an image without running its code");
    provider.profile = ProfileForVersion(version);
    Require(provider.profile != nullptr, "provider profile");
    DescriptorDiscovery discovery{};
    const uintptr_t anchor = FindDescriptorEntry(provider.module, provider.bytes, *provider.profile, &discovery);
    Require(anchor != 0 && RegistrationCountMatches(*provider.profile, discovery.registeredSlots),
        "descriptor table bound to the registration loop");
    provider.table = anchor - provider.profile->temporalSlot * sizeof(uintptr_t);
    provider.slots = discovery.registeredSlots;
    std::cout << "version=310.9." << build << " table_rva=0x" << std::hex
        << provider.table - reinterpret_cast<uintptr_t>(provider.module) << std::dec << " slots=" << provider.slots << '\n';
    return provider;
}

ampere_native_cache::CompleteProgram Transform(const Provider& provider)
{
    ampere_native_cache::CompleteProgram program(provider.slots);
    for (size_t i = 0; i < provider.slots; ++i)
    {
        uintptr_t descriptor = 0, fatbin = 0;
        Require(SafeRead(provider.table + i * sizeof(uintptr_t), descriptor) && SafeRead(descriptor + 8, fatbin),
            "descriptor read");
        // Descriptor sizes are cold in an image that never ran; the container header gives the size.
        const uint64_t bytes = ReadU16(reinterpret_cast<const uint8_t*>(fatbin + 6))
            + ReadU64(reinterpret_cast<const uint8_t*>(fatbin + 8));
        Sm89PtxMetadata meta{};
        Require(FindUniqueSm89Ptx(fatbin, bytes, meta), "unique SM89 PTX image");
        const auto* temporal = i == provider.profile->temporalSlot ? provider.profile : nullptr;
        const size_t capacity = meta.outerHeaderSize + meta.imageHeaderSize + ((meta.rawPtxBytes + 7) & ~size_t{7})
            + (temporal ? kTemporalGrowthAllowance : 0);
        program[i].resize(capacity);
        std::vector<uint8_t> scratch(meta.compressedSize);
        uint32_t written = 0;
        StripFailure failure{};
        Require(BuildAmpereSm86Fatbin(fatbin, bytes, meta, program[i].data(), capacity, scratch.data(), scratch.size(),
            written, failure, temporal, OutputPullMaskSlot(*provider.profile, i)), "production SM86 transform");
        program[i].resize(written);
    }
    return program;
}
}

int wmain(int argc, wchar_t** argv)
{
    namespace cache = ampere_native_cache;
    try
    {
        if (argc != 3) throw std::runtime_error("usage: audit 310.9.0.dll 310.9.16.dll");
        Require(ProfileForVersion({310, 9, 16}) == &k310916TemporalProfile && !ProfileForVersion({310, 9, 15})
            && !ProfileForVersion({310, 9, 17}), "310.9.16 binds the 26-name RTX 30 profile");
        Require(OutputPullMaskSlot(k310916TemporalProfile, 15) && !OutputPullMaskSlot(k310916TemporalProfile, 14)
            && OutputPullMaskSlot(k3109TemporalProfile, 14) && !OutputPullMaskSlot(k3109TemporalProfile, 15),
            "OutputPull mask slot follows the profile");
        Require(cache::HasManifest() && cache::ConfiguredMode() == cache::Mode::eAuto, "embedded native caches in AUTO mode");

        const auto old = Map(argv[1], 0);
        const auto now = Map(argv[2], 16);
        Require(old.slots == 25 && now.slots == 26, "registration loops bind 25 and 26 slots");
        Require(!FindDescriptorEntry(now.module, now.bytes, k3109TemporalProfile)
            && !FindDescriptorEntry(old.module, old.bytes, k310916TemporalProfile), "each profile rejects the other table");

        const auto oldProgram = Transform(old);
        const auto newProgram = Transform(now);
        Require(newProgram[10] == oldProgram[9], "midpoint program identical to 310.9.0's");
        Require(newProgram[15] == oldProgram[14], "masked OutputPull program identical to 310.9.0's");
        Require(newProgram[0] != oldProgram[0] && newProgram[9] != oldProgram[8],
            "changed BlendCandidatesFused and DownsampleRGBAndEstimateUIFused programs");
        size_t shared = 0;
        for (size_t i = 2; i < newProgram.size(); ++i) shared += newProgram[i] == oldProgram[i - 1];
        Require(shared == 23, "the other 23 programs are 310.9.0's, one slot later");

        cache::CompleteProgram native;
        Require(cache::LoadConfiguredProgram(newProgram, native) == cache::Status::eLoaded && native.size() == 26,
            "310.9.16 selects the complete embedded native SM86 program");
        for (const auto& slot : native) Require(cache::ValidateNativeContainer(slot), "native SM86 container");
        cache::CompleteProgram oldNative;
        Require(cache::LoadConfiguredProgram(oldProgram, oldNative) == cache::Status::eLoaded && oldNative.size() == 25,
            "310.9.0 keeps its native program");

        cache::CompleteProgram rejected;
        auto swapped = newProgram;
        std::swap(swapped[0], swapped[1]);
        Require(cache::LoadConfiguredProgram(swapped, rejected) == cache::Status::eSourceMismatch, "swapped slots rejected");
        auto mutated = newProgram;
        mutated[1][mutated[1].size() / 2] ^= 1;
        Require(cache::LoadConfiguredProgram(mutated, rejected) == cache::Status::eSourceMismatch, "mutated FG Lite program rejected");
        auto prefix = newProgram;
        prefix.pop_back();
        Require(cache::LoadConfiguredProgram(prefix, rejected) == cache::Status::eSourceMismatch, "25-slot prefix rejected");
        std::cout << "AMPERE_310916_AUDIT_PASSED\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << "AUDIT_FAILED: " << error.what() << '\n';
        return 1;
    }
}
