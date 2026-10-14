#include "ampere_provider_arch.h"
#include <psapi.h>
#include <iostream>
#include <cstring>
unsigned checks = 0, failures = 0;
void Check(bool ok, const char* text)
{ ++checks; failures += !ok; std::cout << (ok ? "PASS " : "FAIL ") << text << '\n'; }
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
    MEMORY_BASIC_INFORMATION memory{};
    Check(VirtualQuery(code, &memory, sizeof(memory)) == sizeof(memory) && memory.Protect == PAGE_EXECUTE_READ,
        "declaration restores executable page protection and verifies readback");
    Check(Declare(provider) == Outcome::eAlreadyDeclared && Read().declared == 1 && Read().alreadyDeclared == 1,
        "repeated declaration is idempotent and retains publication evidence");
    FreeLibrary(sibling); FreeLibrary(provider);
    provider = LoadLibraryExW(argv[1], nullptr, DONT_RESOLVE_DLL_REFERENCES);
    code = provider ? reinterpret_cast<uint8_t*>(GetProcAddress(provider, kArchitectureExport)) : nullptr;
    Check(ArchitectureExportIs(code, kAdaArchitecture), "unmapping and remapping sees the unchanged original provider file");
    if (provider) FreeLibrary(provider);
    std::cout << checks << " checks, " << failures << " failures\n";
    return failures ? 1 : 0;
}
