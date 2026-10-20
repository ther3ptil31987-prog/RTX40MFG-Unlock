#include "ampere_provider_arch.h"
#include "adapter_discovery.h"
#include "dlssg_provider_policy.h"
#include "protected_pointer.h"
#include "third_party/minhook/src/hde/hde64.h"
#include <psapi.h>
#include <array>
#include <atomic>

namespace ampere_provider_arch
{
namespace
{
std::atomic<bool> gDetected{false};
std::atomic<bool> gArmed{false};
std::atomic<uint32_t> gArchitecture{0};
std::atomic<uint32_t> gImplementation{0};
std::atomic<uint32_t> gPhysicalGpuCount{0}, gSingleGpuArchitecture{0}, gSingleGpuImplementation{0};
std::atomic<uint32_t> gNvapiRedirected{0};
std::atomic<uint64_t> gDeclared{0};
std::atomic<uint64_t> gAlreadyDeclared{0};
std::atomic<uint64_t> gShapeMismatches{0};
std::atomic<uint64_t> gPublicationFailures{0};
std::atomic<uint32_t> gLast{static_cast<uint32_t>(Outcome::eNotArmed)};
std::atomic<HMODULE> gLastModule{nullptr};
std::atomic<uint32_t> gLastVulkan{static_cast<uint32_t>(VulkanOutcome::eNotChecked)};
std::atomic<uint64_t> gVulkanDeclared{0};

constexpr size_t kMaximumRequirementsBytes = 0x4000;

VulkanOutcome RecordVulkan(VulkanOutcome outcome) noexcept
{
    if (outcome == VulkanOutcome::eDeclared) gVulkanDeclared.fetch_add(1, std::memory_order_relaxed);
    gLastVulkan.store(static_cast<uint32_t>(outcome), std::memory_order_release);
    return outcome;
}

Outcome Record(Outcome outcome, HMODULE module) noexcept
{
    switch (outcome)
    {
    case Outcome::eDeclared: gDeclared.fetch_add(1, std::memory_order_relaxed); break;
    case Outcome::eAlreadyDeclared: gAlreadyDeclared.fetch_add(1, std::memory_order_relaxed); break;
    case Outcome::eShapeMismatch: gShapeMismatches.fetch_add(1, std::memory_order_relaxed); break;
    case Outcome::ePublicationFailed: gPublicationFailures.fetch_add(1, std::memory_order_relaxed); break;
    default: return outcome;
    }
    gLastModule.store(module, std::memory_order_relaxed);
    gLast.store(static_cast<uint32_t>(outcome), std::memory_order_release);
    return outcome;
}

bool ReadableImage(HMODULE module, const uint8_t* begin, size_t size) noexcept
{
    MEMORY_BASIC_INFORMATION memory{};
    if (!begin || !size || VirtualQuery(begin, &memory, sizeof(memory)) != sizeof(memory)
        || memory.AllocationBase != module || memory.Type != MEM_IMAGE || memory.State != MEM_COMMIT
        || (memory.Protect & (PAGE_GUARD | PAGE_NOACCESS)))
        return false;
    const DWORD protection = memory.Protect & 0xFFu;
    const auto region = reinterpret_cast<uintptr_t>(memory.BaseAddress);
    const auto start = reinterpret_cast<uintptr_t>(begin);
    return (protection == PAGE_EXECUTE_READ || protection == PAGE_EXECUTE)
        && start >= region && size <= region + memory.RegionSize - start;
}

// The image's own exception directory bounds the export's function. It is read
// directly because a loader notification can precede the loader's insertion of
// the image into the process function table used by RtlLookupFunctionEntry.
bool FunctionRange(HMODULE module, const uint8_t* entry, uint32_t& begin, uint32_t& end) noexcept
{
    const auto* base = reinterpret_cast<const uint8_t*>(module);
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (!module || dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0 || dos->e_lfanew > 0x100000) return false;
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE || nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC
        || nt->OptionalHeader.NumberOfRvaAndSizes <= IMAGE_DIRECTORY_ENTRY_EXCEPTION) return false;
    const size_t imageBytes = nt->OptionalHeader.SizeOfImage;
    const auto& directory = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION];
    if (!directory.VirtualAddress || directory.Size < sizeof(RUNTIME_FUNCTION)
        || directory.VirtualAddress >= imageBytes || directory.Size > imageBytes - directory.VirtualAddress
        || entry < base || static_cast<size_t>(entry - base) >= imageBytes) return false;
    const auto rva = static_cast<uint32_t>(entry - base);
    const auto* functions = reinterpret_cast<const RUNTIME_FUNCTION*>(base + directory.VirtualAddress);
    size_t low = 0, high = directory.Size / sizeof(RUNTIME_FUNCTION);
    while (low < high)
    {
        const size_t middle = low + (high - low) / 2;
        if (rva < functions[middle].BeginAddress) high = middle;
        else if (rva >= functions[middle].EndAddress) low = middle + 1;
        else
        {
            begin = functions[middle].BeginAddress;
            end = functions[middle].EndAddress;
            return begin == rva && end > begin && end <= imageBytes;
        }
    }
    return false;
}

// A page already made private by an earlier entry detour cannot be re-armed as
// WRITECOPY. Select the private-page writable form only when Windows reports it.
bool PublishByte(uint8_t* address, uint8_t expected, uint8_t replacement) noexcept
{
    PSAPI_WORKING_SET_EX_INFORMATION working{};
    working.VirtualAddress = address;
    const bool alreadyPrivate = QueryWorkingSetEx(GetCurrentProcess(), &working, sizeof(working))
        && working.VirtualAttributes.Valid && !working.VirtualAttributes.Shared;
    const auto result = protected_pointer::ReplaceProtectedBytes(reinterpret_cast<uintptr_t>(address),
        &expected, &replacement, 1, PAGE_EXECUTE_READ, &VirtualProtect, &FlushInstructionCache,
        alreadyPrivate ? PAGE_EXECUTE_READWRITE : PAGE_EXECUTE_WRITECOPY);
    return result.disposition == protected_pointer::PublishDisposition::ePublishedRestored
        && *address == replacement;
}

// NVAPI is resolved as for adapter discovery (adapter_discovery::ResolveNvapi).
// Any failure detects nothing.
bool SystemAmpere(uint32_t& architecture, uint32_t& implementation,
    uint32_t& physicalCount, uint32_t& singleArchitecture, uint32_t& singleImplementation) noexcept
{
    using namespace adapter_discovery;
    constexpr std::array<uint32_t, 3> ids{0x0150e828, 0xe5ac921f, 0xd8265d24};
    NvapiTargets<ids.size()> resolved;
    if (!ResolveNvapi(ids, resolved)) return false;
    gNvapiRedirected.store(resolved.redirected, std::memory_order_relaxed);
    const auto initialize = reinterpret_cast<Initialize>(resolved.targets[0]);
    const auto enumerate = reinterpret_cast<Enumerate>(resolved.targets[1]);
    const auto getArchitecture = reinterpret_cast<GetArchitecture>(resolved.targets[2]);
    if (initialize() != 0) return false;
    std::array<void*, 64> physical{};
    uint32_t count = 0;
    if (enumerate(physical.data(), &count) != 0 || !count || count > physical.size()) return false;
    physicalCount = count;
    bool ampere = false;
    for (uint32_t i = 0; i < count; ++i)
    {
        Architecture info{sizeof(Architecture) | (2u << 16), 0, 0, 0};
        if (!physical[i] || getArchitecture(physical[i], &info) != 0) continue;
        if (count == 1)
        {
            singleArchitecture = info.architecture;
            singleImplementation = info.implementation;
        }
        if (IsTranslatedRtx(info.architecture, info.implementation)
            && (!ampere || info.architecture < architecture))
        {
            architecture = info.architecture;
            implementation = info.implementation;
            ampere = true;
        }
    }
    return ampere;
}
}

size_t FindRequirementsArchitecture(const uint8_t* begin, size_t size) noexcept
{
    struct Store { size_t offset; uint8_t displacement; uint32_t value; };
    std::array<Store, 64> stores{};
    size_t count = 0;
    if (!begin || !size || size > kMaximumRequirementsBytes) return SIZE_MAX;
    for (size_t cursor = 0; cursor < size;)
    {
        hde64s instruction{};
        const unsigned length = hde64_disasm(begin + cursor, &instruction);
        if (!length || (instruction.flags & F_ERROR) || length > size - cursor) return SIZE_MAX;
        // mov dword [rsp+disp8], imm32 (C7 44 24 d8 imm32), no prefixes.
        if (length == 8 && instruction.opcode == 0xC7 && !(instruction.flags & F_PREFIX_ANY)
            && instruction.modrm == 0x44 && (instruction.flags & F_SIB) && instruction.sib == 0x24
            && (instruction.flags & F_DISP8) && (instruction.flags & F_IMM32))
        {
            if (count == stores.size()) return SIZE_MAX;
            stores[count++] = {cursor, instruction.disp.disp8, instruction.imm.imm32};
        }
        cursor += length;
    }
    size_t found = SIZE_MAX;
    unsigned architectureStores = 0;
    for (size_t i = 0; i < count; ++i)
    {
        if (stores[i].value != kAdaArchitecture && stores[i].value != kAmpereArchitecture && stores[i].value != kTuringArchitecture) continue;
        ++architectureStores;
        for (size_t j = 0; j < i; ++j)
        {
            if (stores[j].value == kFrameGenerationFeature
                && stores[j].displacement == static_cast<uint8_t>(stores[i].displacement - 4))
                found = stores[i].offset + 4;
        }
    }
    return architectureStores == 1 ? found : SIZE_MAX;
}

const char* OutcomeName(Outcome outcome) noexcept
{
    switch (outcome)
    {
    case Outcome::eNotArmed: return "not-armed";
    case Outcome::eNotProvider: return "not-provider";
    case Outcome::eDeclared: return "declared";
    case Outcome::eAlreadyDeclared: return "already-declared";
    case Outcome::eShapeMismatch: return "shape-mismatch";
    case Outcome::ePublicationFailed: return "publication-failed";
    default: return "unknown";
    }
}

bool DetectAndArm() noexcept
{
    if (gDetected.load(std::memory_order_acquire)) return gArmed.load(std::memory_order_acquire);
    uint32_t architecture = 0, implementation = 0;
    uint32_t count = 0, singleArchitecture = 0, singleImplementation = 0;
    const bool ampere = SystemAmpere(architecture, implementation, count, singleArchitecture, singleImplementation);
    if (ampere) Arm(architecture, implementation);
    gPhysicalGpuCount.store(count, std::memory_order_relaxed);
    gSingleGpuArchitecture.store(singleArchitecture, std::memory_order_relaxed);
    gSingleGpuImplementation.store(singleImplementation, std::memory_order_relaxed);
    gDetected.store(true, std::memory_order_release);
    return ampere;
}

void Arm(uint32_t architecture, uint32_t implementation) noexcept
{
    if (!IsTranslatedRtx(architecture, implementation)) return;
    gArchitecture.store(architecture, std::memory_order_relaxed);
    gImplementation.store(implementation, std::memory_order_relaxed);
    gArmed.store(true, std::memory_order_release);
}

bool Armed() noexcept { return gArmed.load(std::memory_order_acquire); }
bool Detected() noexcept { return gDetected.load(std::memory_order_acquire); }

const char* VulkanOutcomeName(VulkanOutcome outcome) noexcept
{
    switch (outcome)
    {
    case VulkanOutcome::eNotChecked: return "not-checked";
    case VulkanOutcome::eDeclared: return "declared";
    case VulkanOutcome::eAlreadyDeclared: return "already-declared";
    case VulkanOutcome::eAbsent: return "absent";
    case VulkanOutcome::eShapeMismatch: return "shape-mismatch";
    case VulkanOutcome::ePublicationFailed: return "publication-failed";
    default: return "unknown";
    }
}

namespace
{
// The architecture immediate of one requirements export, or null when the
// export is absent (present=false) or not the recognized record shape.
uint8_t* RequirementsImmediate(HMODULE module, const char* name, bool& present) noexcept
{
    auto* requirements = reinterpret_cast<uint8_t*>(GetProcAddress(module, name));
    present = requirements != nullptr;
    uint32_t begin = 0, end = 0;
    if (!requirements || !FunctionRange(module, requirements, begin, end)
        || end - begin > kMaximumRequirementsBytes || !ReadableImage(module, requirements, end - begin))
        return nullptr;
    const size_t offset = FindRequirementsArchitecture(requirements, end - begin);
    return offset == SIZE_MAX ? nullptr : requirements + offset;
}

bool ImmediateIs(const uint8_t* immediate, uint32_t value) noexcept
{
    return immediate && immediate[0] == static_cast<uint8_t>(value)
        && immediate[1] == static_cast<uint8_t>(value >> 8) && immediate[2] == 0 && immediate[3] == 0;
}

// Vulkan record after the D3D12 one; never changes the D3D12 outcome.
void DeclareVulkanRequirements(HMODULE module, uint32_t target) noexcept
{
    bool present = false;
    uint8_t* const immediate = RequirementsImmediate(module, kVulkanRequirementsExport, present);
    if (!present) { RecordVulkan(VulkanOutcome::eAbsent); return; }
    if (ImmediateIs(immediate, target)) { RecordVulkan(VulkanOutcome::eAlreadyDeclared); return; }
    if (!ImmediateIs(immediate, kAdaArchitecture)) { RecordVulkan(VulkanOutcome::eShapeMismatch); return; }
    RecordVulkan(PublishByte(immediate, 0x90, static_cast<uint8_t>(target)) && ImmediateIs(immediate, target)
        ? VulkanOutcome::eDeclared : VulkanOutcome::ePublicationFailed);
}
}

Outcome Declare(HMODULE module) noexcept
{
    if (!gArmed.load(std::memory_order_acquire)) return Outcome::eNotArmed;
    if (!module || !dlssg_provider_policy::IsDlssgImplementationModule(module)) return Outcome::eNotProvider;
    const uint32_t target = gArchitecture.load(std::memory_order_acquire);
    if (target != kAmpereArchitecture && target != kTuringArchitecture) return Outcome::eNotArmed;
    auto* exportCode = reinterpret_cast<uint8_t*>(GetProcAddress(module, kArchitectureExport));
    if (!exportCode || !ReadableImage(module, exportCode, 6))
        return Record(Outcome::eShapeMismatch, module);
    bool present = false;
    uint8_t* const requirementImmediate = RequirementsImmediate(module, kRequirementsExport, present);
    if (!requirementImmediate) return Record(Outcome::eShapeMismatch, module);
    const bool exportAda = ArchitectureExportIs(exportCode, kAdaArchitecture);
    const bool exportAmpere = ArchitectureExportIs(exportCode, target);
    const bool requirementAda = ImmediateIs(requirementImmediate, kAdaArchitecture);
    const bool requirementAmpere = ImmediateIs(requirementImmediate, target);
    if ((!exportAda && !exportAmpere) || (!requirementAda && !requirementAmpere))
        return Record(Outcome::eShapeMismatch, module);
    // 0x190 and 0x170 differ only in the low byte. Requirements first: an
    // interrupted publication then leaves NGX's own snippet gate closed.
    if (requirementAda && !PublishByte(requirementImmediate, 0x90, static_cast<uint8_t>(target)))
        return Record(Outcome::ePublicationFailed, module);
    DeclareVulkanRequirements(module, target);
    if (exportAmpere && requirementAmpere) return Record(Outcome::eAlreadyDeclared, module);
    if (exportAda && !PublishByte(exportCode + 1, 0x90, static_cast<uint8_t>(target)))
        return Record(Outcome::ePublicationFailed, module);
    return Record(ArchitectureExportIs(exportCode, target) && ImmediateIs(requirementImmediate, target)
        ? Outcome::eDeclared : Outcome::ePublicationFailed, module);
}

Snapshot Read() noexcept
{
    Snapshot snapshot{};
    snapshot.detected = gDetected.load(std::memory_order_acquire);
    snapshot.armed = gArmed.load(std::memory_order_acquire);
    snapshot.gpuArchitecture = gArchitecture.load(std::memory_order_relaxed);
    snapshot.gpuImplementation = gImplementation.load(std::memory_order_relaxed);
    snapshot.physicalGpuCount = gPhysicalGpuCount.load(std::memory_order_relaxed);
    snapshot.singleGpuArchitecture = gSingleGpuArchitecture.load(std::memory_order_relaxed);
    snapshot.singleGpuImplementation = gSingleGpuImplementation.load(std::memory_order_relaxed);
    snapshot.declared = gDeclared.load(std::memory_order_relaxed);
    snapshot.alreadyDeclared = gAlreadyDeclared.load(std::memory_order_relaxed);
    snapshot.shapeMismatches = gShapeMismatches.load(std::memory_order_relaxed);
    snapshot.publicationFailures = gPublicationFailures.load(std::memory_order_relaxed);
    snapshot.last = static_cast<Outcome>(gLast.load(std::memory_order_acquire));
    snapshot.lastModule = gLastModule.load(std::memory_order_relaxed);
    snapshot.nvapiRedirected = gNvapiRedirected.load(std::memory_order_relaxed);
    snapshot.lastVulkan = static_cast<VulkanOutcome>(gLastVulkan.load(std::memory_order_acquire));
    snapshot.vulkanDeclared = gVulkanDeclared.load(std::memory_order_relaxed);
    return snapshot;
}
}
