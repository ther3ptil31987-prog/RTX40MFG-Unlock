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
std::atomic<uint64_t> gDeclared{0};
std::atomic<uint64_t> gAlreadyDeclared{0};
std::atomic<uint64_t> gShapeMismatches{0};
std::atomic<uint64_t> gPublicationFailures{0};
std::atomic<uint32_t> gLast{static_cast<uint32_t>(Outcome::eNotArmed)};
std::atomic<HMODULE> gLastModule{nullptr};

constexpr size_t kMaximumRequirementsBytes = 0x4000;

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

// NVAPI is loaded only from System32 and its factory-owned code from the
// trusted driver store, as for adapter discovery. Any failure detects nothing.
bool SystemAmpere(uint32_t& architecture, uint32_t& implementation,
    uint32_t& physicalCount, uint32_t& singleArchitecture, uint32_t& singleImplementation) noexcept
{
    using namespace adapter_discovery;
    std::array<wchar_t, kPathCapacity> systemDirectory{}, stubPath{}, loadedPath{};
    const UINT systemLength = GetSystemDirectoryW(systemDirectory.data(), static_cast<UINT>(systemDirectory.size()));
    if (!systemLength || systemLength >= systemDirectory.size()
        || swprintf_s(stubPath.data(), stubPath.size(), L"%ls\\nvapi64.dll", systemDirectory.data()) < 0) return false;
    ModuleReference dispatcher;
    dispatcher.module = LoadLibraryExW(stubPath.data(), nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!dispatcher.module) return false;
    // Never be the reference which unmaps NVAPI: hooks other modules placed on
    // its first load (OptiScaler's QueryInterface detour) would go with it.
    HMODULE pinnedDispatcher = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
        reinterpret_cast<LPCWSTR>(dispatcher.module), &pinnedDispatcher);
    const DWORD stubLength = GetModuleFileNameW(dispatcher.module, loadedPath.data(), static_cast<DWORD>(loadedPath.size()));
    if (!stubLength || stubLength >= loadedPath.size()
        || !NvapiOwnerPath(systemDirectory.data(), loadedPath.data(), true)) return false;
    const auto query = reinterpret_cast<QueryInterface>(GetProcAddress(dispatcher.module, "nvapi_QueryInterface"));
    if (!OwnedCode(dispatcher.module, reinterpret_cast<void*>(query))) return false;
    constexpr std::array<uint32_t, 3> ids{0x0150e828, 0xe5ac921f, 0xd8265d24};
    std::array<void*, ids.size()> targets{};
    std::array<ModuleReference, ids.size()> owners{};
    for (size_t i = 0; i < ids.size(); ++i)
    {
        targets[i] = query(ids[i]);
        if (!owners[i].Acquire(targets[i])) return false;
        const DWORD ownerLength = GetModuleFileNameW(owners[i].module, loadedPath.data(), static_cast<DWORD>(loadedPath.size()));
        if (!ownerLength || ownerLength >= loadedPath.size()
            || !NvapiOwnerPath(systemDirectory.data(), loadedPath.data())
            || !OwnerFactoryMatches(owners[i].module, ids[i], targets[i])) return false;
    }
    const auto initialize = reinterpret_cast<Initialize>(targets[0]);
    const auto enumerate = reinterpret_cast<Enumerate>(targets[1]);
    const auto getArchitecture = reinterpret_cast<GetArchitecture>(targets[2]);
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
        if (!ampere && IsAmpereRtx(info.architecture, info.implementation))
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
        if (stores[i].value != kAdaArchitecture && stores[i].value != kAmpereArchitecture) continue;
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
    if (!IsAmpereRtx(architecture, implementation)) return;
    gArchitecture.store(architecture, std::memory_order_relaxed);
    gImplementation.store(implementation, std::memory_order_relaxed);
    gArmed.store(true, std::memory_order_release);
}

bool Armed() noexcept { return gArmed.load(std::memory_order_acquire); }
bool Detected() noexcept { return gDetected.load(std::memory_order_acquire); }

Outcome Declare(HMODULE module) noexcept
{
    if (!gArmed.load(std::memory_order_acquire)) return Outcome::eNotArmed;
    if (!module || !dlssg_provider_policy::IsDlssgImplementationModule(module)) return Outcome::eNotProvider;
    auto* exportCode = reinterpret_cast<uint8_t*>(GetProcAddress(module, kArchitectureExport));
    auto* requirements = reinterpret_cast<uint8_t*>(GetProcAddress(module, kRequirementsExport));
    if (!exportCode || !requirements || !ReadableImage(module, exportCode, 6))
        return Record(Outcome::eShapeMismatch, module);
    uint32_t begin = 0, end = 0;
    if (!FunctionRange(module, requirements, begin, end) || end - begin > kMaximumRequirementsBytes
        || !ReadableImage(module, requirements, end - begin))
        return Record(Outcome::eShapeMismatch, module);
    const size_t offset = FindRequirementsArchitecture(requirements, end - begin);
    if (offset == SIZE_MAX) return Record(Outcome::eShapeMismatch, module);
    uint8_t* const requirementImmediate = requirements + offset;
    const bool exportAda = ArchitectureExportIs(exportCode, kAdaArchitecture);
    const bool exportAmpere = ArchitectureExportIs(exportCode, kAmpereArchitecture);
    const auto immediateIs = [requirementImmediate](uint32_t value) {
        return requirementImmediate[0] == static_cast<uint8_t>(value)
            && requirementImmediate[1] == static_cast<uint8_t>(value >> 8)
            && requirementImmediate[2] == 0 && requirementImmediate[3] == 0;
    };
    const bool requirementAda = immediateIs(kAdaArchitecture);
    const bool requirementAmpere = immediateIs(kAmpereArchitecture);
    if ((!exportAda && !exportAmpere) || (!requirementAda && !requirementAmpere))
        return Record(Outcome::eShapeMismatch, module);
    if (exportAmpere && requirementAmpere) return Record(Outcome::eAlreadyDeclared, module);
    // 0x190 and 0x170 differ only in the low byte. Requirements first: an
    // interrupted publication then leaves NGX's own snippet gate closed.
    if ((requirementAda && !PublishByte(requirementImmediate, 0x90, 0x70))
        || (exportAda && !PublishByte(exportCode + 1, 0x90, 0x70)))
        return Record(Outcome::ePublicationFailed, module);
    return Record(ArchitectureExportIs(exportCode, kAmpereArchitecture) && immediateIs(kAmpereArchitecture)
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
    return snapshot;
}
}
