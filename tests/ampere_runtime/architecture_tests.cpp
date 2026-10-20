#include "ampere_provider_arch.h"
#include <psapi.h>
#include <iostream>
#include <cstring>
unsigned checks = 0, failures = 0;
void Check(bool ok, const char* text)
{ ++checks; failures += !ok; std::cout << (ok ? "PASS " : "FAIL ") << text << '\n'; }

// The requirements record's architecture immediate in one export, bounded by
// the image's own exception directory (as the declaration bounds it).
uint32_t RequirementsArchitecture(HMODULE module, const char* name, bool& present)
{
    auto* entry = reinterpret_cast<const uint8_t*>(GetProcAddress(module, name));
    present = entry != nullptr;
    if (!entry) return 0;
    const auto* base = reinterpret_cast<const uint8_t*>(module);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base
        + reinterpret_cast<const IMAGE_DOS_HEADER*>(base)->e_lfanew);
    const auto& directory = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION];
    const auto* functions = reinterpret_cast<const RUNTIME_FUNCTION*>(base + directory.VirtualAddress);
    const auto rva = static_cast<uint32_t>(entry - base);
    for (size_t i = 0; i < directory.Size / sizeof(RUNTIME_FUNCTION); ++i)
    {
        if (functions[i].BeginAddress != rva) continue;
        const size_t offset = ampere_provider_arch::FindRequirementsArchitecture(entry,
            functions[i].EndAddress - functions[i].BeginAddress);
        if (offset == SIZE_MAX) return 0;
        uint32_t value = 0;
        std::memcpy(&value, entry + offset, sizeof(value));
        return value;
    }
    return 0;
}
int wmain(int argc, wchar_t** argv)
{
    if (argc != 4) return 2;
    using namespace ampere_provider_arch;
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    HMODULE provider = LoadLibraryExW(argv[1], nullptr, DONT_RESOLVE_DLL_REFERENCES);
    HMODULE sibling = LoadLibraryExW(argv[2], nullptr, DONT_RESOLVE_DLL_REFERENCES);
    Check(provider && sibling, "exact provider and sibling map without plugin startup or GPU execution");
    if (!provider || !sibling) return 2;
    auto* code = reinterpret_cast<uint8_t*>(GetProcAddress(provider, kArchitectureExport));
    Check(ArchitectureExportIs(code, kAdaArchitecture), "provider starts with the expected Ada architecture export");
    bool d3d12Present = false, vulkanPresent = false;
    Check(RequirementsArchitecture(provider, kRequirementsExport, d3d12Present) == kAdaArchitecture,
        "provider's D3D12 requirements record starts at Ada");
    const uint32_t vulkanBefore = RequirementsArchitecture(provider, kVulkanRequirementsExport, vulkanPresent);
    std::cout << "INFO Vulkan requirements export present=" << vulkanPresent << " architecture=0x"
        << std::hex << vulkanBefore << std::dec << '\n';
    Check(!vulkanPresent || vulkanBefore == kAdaArchitecture,
        "provider's Vulkan requirements record, when exported, is the recognized Ada record");
    Check(Declare(provider) == Outcome::eNotArmed && ArchitectureExportIs(code, kAdaArchitecture),
        "unknown adapter cannot modify an eligible mapped provider");
    Arm(kAdaArchitecture, 2);
    Arm(kAmpereArchitecture, 0);
    Check(!Armed(), "Ada and non-RTX Ampere cannot arm the declaration");
    if (std::wstring(argv[3]) == L"private")
    {
        DWORD protection = 0, ignored = 0;
        const bool writable = VirtualProtect(code, 6, PAGE_EXECUTE_WRITECOPY, &protection) != FALSE;
        Check(writable, "fixture acquires a copy-on-write image page");
        if (!writable) return 2;
        // Writing the same byte forces a private page, as an earlier verified
        // entry detour would, without changing the provider's instructions.
        *reinterpret_cast<volatile uint8_t*>(code + 1) = code[1];
        Check(VirtualProtect(code, 6, protection, &ignored) != FALSE,
            "fixture restores original executable protection");
        PSAPI_WORKING_SET_EX_INFORMATION page{}; page.VirtualAddress = code;
        Check(QueryWorkingSetEx(GetCurrentProcess(), &page, sizeof(page))
            && page.VirtualAttributes.Valid && !page.VirtualAttributes.Shared,
            "declaration sees an already-private provider code page");
    }
    Arm(kAmpereArchitecture, 4);
    Check(Armed() && Read().gpuImplementation == 4, "established RTX Ampere architecture arms the isolated harness");
    Check(Declare(sibling) == Outcome::eNotProvider, "Super Resolution sibling cannot receive the FG declaration");
    Check(Declare(provider) == Outcome::eDeclared && ArchitectureExportIs(code, kAmpereArchitecture),
        "both independently verified architecture sites publish Ampere in process memory");
    bool present = false;
    Check(RequirementsArchitecture(provider, kRequirementsExport, present) == kAmpereArchitecture,
        "the D3D12 requirements record now declares Ampere");
    Check(vulkanPresent
        ? Read().lastVulkan == VulkanOutcome::eDeclared && Read().vulkanDeclared == 1
            && RequirementsArchitecture(provider, kVulkanRequirementsExport, present) == kAmpereArchitecture
        : Read().lastVulkan == VulkanOutcome::eAbsent,
        "the Vulkan requirements record declares Ampere with the D3D12 one (or is reported absent)");
    MEMORY_BASIC_INFORMATION memory{};
    Check(VirtualQuery(code, &memory, sizeof(memory)) == sizeof(memory) && memory.Protect == PAGE_EXECUTE_READ,
        "declaration restores executable page protection and verifies readback");
    Check(Declare(provider) == Outcome::eAlreadyDeclared && Read().declared == 1 && Read().alreadyDeclared == 1,
        "repeated declaration is idempotent and retains publication evidence");
    Check(Read().lastVulkan == (vulkanPresent ? VulkanOutcome::eAlreadyDeclared : VulkanOutcome::eAbsent)
        && Read().vulkanDeclared == (vulkanPresent ? 1u : 0u),
        "repeated declaration leaves the Vulkan record declared once");
    FreeLibrary(sibling); FreeLibrary(provider);
    provider = LoadLibraryExW(argv[1], nullptr, DONT_RESOLVE_DLL_REFERENCES);
    code = provider ? reinterpret_cast<uint8_t*>(GetProcAddress(provider, kArchitectureExport)) : nullptr;
    Check(ArchitectureExportIs(code, kAdaArchitecture), "unmapping and remapping sees the unchanged original provider file");
    Check(!provider || !vulkanPresent
        || RequirementsArchitecture(provider, kVulkanRequirementsExport, present) == kAdaArchitecture,
        "the remapped provider file keeps its original Vulkan requirements record");
    if (provider) FreeLibrary(provider);
    std::cout << checks << " checks, " << failures << " failures\n";
    return failures ? 1 : 0;
}
