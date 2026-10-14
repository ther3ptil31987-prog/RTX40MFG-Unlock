#include "dlssg_provider_selection.h"
#include <algorithm>
#include <atomic>
#include <cstring>
#include <filesystem>
#include <vector>
#include <winver.h>
#include <wintrust.h>
#include <softpub.h>
#include <wincrypt.h>

namespace dlssg_provider_selection
{
namespace
{
namespace fs = std::filesystem;
using Version = dlssg_provider_policy::VersionTriplet;
uint64_t Rank(Version v) noexcept { return (uint64_t{v.major} << 32) | (uint64_t{v.minor} << 16) | v.build; }
struct File
{
    HANDLE handle = INVALID_HANDLE_VALUE, mapping = nullptr;
    const uint8_t* data = nullptr;
    size_t bytes = 0;
    std::vector<HANDLE> directories;
    ~File()
    {
        if (data) UnmapViewOfFile(data);
        if (mapping) CloseHandle(mapping);
        if (handle != INVALID_HANDLE_VALUE) CloseHandle(handle);
        for (const auto directory : directories) CloseHandle(directory);
    }
    bool LockParents(const fs::path& path)
    {
        auto current = path.root_path();
        const auto lock = [&](const fs::path& directory) {
            const HANDLE held = CreateFileW(directory.c_str(), FILE_READ_ATTRIBUTES,
                FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
            if (held == INVALID_HANDLE_VALUE) return false;
            BY_HANDLE_FILE_INFORMATION information{};
            if (!GetFileInformationByHandle(held, &information)
                || !(information.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
                || (information.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT))
            { CloseHandle(held); return false; }
            try { directories.push_back(held); }
            catch (...) { CloseHandle(held); throw; }
            return true;
        };
        if (!path.is_absolute() || !lock(current)) return false;
        for (const auto& part : path.relative_path())
        {
            current /= part;
            if (!lock(current)) return false;
        }
        return true;
    }
    bool ExactPath(const std::wstring& expected) const noexcept
    {
        BY_HANDLE_FILE_INFORMATION information{};
        wchar_t final[32768]{};
        const DWORD length = GetFinalPathNameByHandleW(handle, final, static_cast<DWORD>(std::size(final)),
            FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
        return length > 4 && length < std::size(final) && wcsncmp(final, L"\\\\?\\", 4) == 0
            && _wcsicmp(final + 4, expected.c_str()) == 0
            && GetFileInformationByHandle(handle, &information)
            && !(information.dwFileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY));
    }
    bool Open(const std::wstring& path) noexcept
    {
        handle = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        LARGE_INTEGER size{};
        if (handle == INVALID_HANDLE_VALUE || !GetFileSizeEx(handle, &size)
            || size.QuadPart < 4096 || size.QuadPart > 512ll * 1024 * 1024) return false;
        bytes = static_cast<size_t>(size.QuadPart);
        mapping = CreateFileMappingW(handle, nullptr, PAGE_READONLY, 0, 0, nullptr);
        if (!mapping) return false;
        data = static_cast<const uint8_t*>(MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, 0));
        return data != nullptr;
    }
    const uint8_t* At(size_t offset, size_t count) const noexcept
    { return data && offset <= bytes && count <= bytes - offset ? data + offset : nullptr; }
    bool Identity() const noexcept
    {
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(At(0, sizeof(IMAGE_DOS_HEADER)));
        if (!dos || dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0) return false;
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(At(dos->e_lfanew, sizeof(IMAGE_NT_HEADERS64)));
        if (!nt || nt->Signature != IMAGE_NT_SIGNATURE || nt->FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64
            || nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC
            || nt->OptionalHeader.NumberOfRvaAndSizes <= IMAGE_DIRECTORY_ENTRY_EXPORT
            || nt->FileHeader.SizeOfOptionalHeader < sizeof(IMAGE_OPTIONAL_HEADER64)
            || nt->FileHeader.NumberOfSections == 0 || nt->FileHeader.NumberOfSections > 96) return false;
        const auto sectionOffset = static_cast<size_t>(dos->e_lfanew) + offsetof(IMAGE_NT_HEADERS64, OptionalHeader)
            + nt->FileHeader.SizeOfOptionalHeader;
        const auto* sections = reinterpret_cast<const IMAGE_SECTION_HEADER*>(At(sectionOffset,
            nt->FileHeader.NumberOfSections * sizeof(IMAGE_SECTION_HEADER)));
        if (!sections) return false;
        const auto rva = [&](uint32_t address, size_t count) -> const uint8_t* {
            if (address < nt->OptionalHeader.SizeOfHeaders && count <= nt->OptionalHeader.SizeOfHeaders - address)
                return At(address, count);
            for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i)
            {
                const auto& s = sections[i];
                if (address >= s.VirtualAddress && address - s.VirtualAddress <= s.SizeOfRawData
                    && count <= s.SizeOfRawData - (address - s.VirtualAddress))
                    return At(static_cast<size_t>(s.PointerToRawData) + address - s.VirtualAddress, count);
            }
            return nullptr;
        };
        const auto& directory = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
        const auto* exports = reinterpret_cast<const IMAGE_EXPORT_DIRECTORY*>(rva(directory.VirtualAddress, sizeof(IMAGE_EXPORT_DIRECTORY)));
        if (!directory.VirtualAddress || directory.Size < sizeof(IMAGE_EXPORT_DIRECTORY) || !exports
            || !exports->NumberOfNames || exports->NumberOfNames > 65536
            || !exports->NumberOfFunctions || exports->NumberOfFunctions > 65536) return false;
        const auto* names = reinterpret_cast<const DWORD*>(rva(exports->AddressOfNames, exports->NumberOfNames * sizeof(DWORD)));
        const auto* ordinals = reinterpret_cast<const WORD*>(rva(exports->AddressOfNameOrdinals, exports->NumberOfNames * sizeof(WORD)));
        const auto* functions = reinterpret_cast<const DWORD*>(rva(exports->AddressOfFunctions, exports->NumberOfFunctions * sizeof(DWORD)));
        if (!names || !ordinals || !functions) return false;
        bool populate = false, directSr = false;
        for (unsigned i = 0; i < exports->NumberOfNames; ++i)
        {
            if (ordinals[i] >= exports->NumberOfFunctions || !functions[ordinals[i]]) return false;
            // Names must terminate within the file-backed section, not merely
            // within the mapping. Forwarded exports cannot establish ownership.
            size_t length = 0;
            for (; length < 256; ++length)
            {
                if (names[i] > UINT32_MAX - static_cast<uint32_t>(length)) return false;
                const auto* character = rva(names[i] + static_cast<uint32_t>(length), 1);
                if (!character) return false;
                if (!*character) break;
            }
            const auto* name = reinterpret_cast<const char*>(rva(names[i], length + 1));
            if (length == 256 || !name) return false;
            const uint32_t target = functions[ordinals[i]];
            const bool owned = target < directory.VirtualAddress || target - directory.VirtualAddress >= directory.Size;
            if (std::strcmp(name, dlssg_provider_policy::kD3d12ImplementationExport) == 0) populate = owned && rva(target, 1);
            if (std::strcmp(name, dlssg_provider_policy::kDirectSrImplementationExport) == 0) directSr = true;
        }
        return dlssg_provider_policy::HasDlssgExportIdentity(populate, directSr);
    }
};
bool InspectLocked(const std::wstring& path, File& file, Candidate& candidate)
{
    candidate = {};
    Version version{};
    if (!file.Open(path) || !file.Identity() || !dlssg_provider_policy::ReadProviderVersion(path.c_str(), version)) return false;
    candidate = {fs::absolute(path).wstring(), version};
    return true;
}
bool MappedVersion(HMODULE module, Version& version) noexcept
{
    version = {};
    if (!module) return false;
    __try
    {
        HRSRC resource = FindResourceW(module, MAKEINTRESOURCEW(VS_VERSION_INFO), RT_VERSION);
        const DWORD bytes = resource ? SizeofResource(module, resource) : 0;
        HGLOBAL loaded = resource ? LoadResource(module, resource) : nullptr;
        const auto* data = loaded ? static_cast<const uint8_t*>(LockResource(loaded)) : nullptr;
        MEMORY_BASIC_INFORMATION owner{};
        if (!data || bytes < sizeof(VS_FIXEDFILEINFO)
            || VirtualQuery(data, &owner, sizeof(owner)) != sizeof(owner)
            || owner.AllocationBase != module || owner.Type != MEM_IMAGE || owner.State != MEM_COMMIT
            || (owner.Protect & (PAGE_GUARD | PAGE_NOACCESS))) return false;
        VS_FIXEDFILEINFO* fixed = nullptr; UINT count = 0;
        if (!VerQueryValueW(data, L"\\", reinterpret_cast<void**>(&fixed), &count)
            || !fixed || count < sizeof(*fixed)
            || reinterpret_cast<const uint8_t*>(fixed) < data
            || reinterpret_cast<const uint8_t*>(fixed) - data > bytes - sizeof(*fixed)
            || fixed->dwSignature != VS_FFI_SIGNATURE) return false;
        version = {HIWORD(fixed->dwFileVersionMS), LOWORD(fixed->dwFileVersionMS), HIWORD(fixed->dwFileVersionLS)};
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

constexpr wchar_t kOtaVersionsDirectory[] = L"C:\\ProgramData\\NVIDIA\\NGX\\models\\dlssg\\versions";
std::atomic<std::shared_ptr<const Candidate>> gSelected{};
std::atomic<std::shared_ptr<const std::wstring>> gExecutableDirectory{}, gVersionsDirectory{};
std::atomic<bool> gPreparing{false}, gPrepared{false}, gScanLimited{false}, gNativePinned{false};
std::atomic<uint64_t> gAttempts{0}, gLoaded{0}, gFailures{0};
std::atomic<uint32_t> gExamined{0}, gSignatureRejections{0};

bool NativeProviderPinned() noexcept
{
    // Opt-in diagnostics for exact-provider test runs. This only disables OTA
    // substitution; all native provider eligibility/patching gates still apply.
    wchar_t value[2]{};
    return GetEnvironmentVariableW(L"RTXMFG_DIAGNOSTIC_NATIVE_DLSSG", value, 2) == 1 && value[0] == L'1';
}
bool NvidiaSignature(const std::wstring& path, const File& file) noexcept
{
    WINTRUST_FILE_INFO subject{};
    subject.cbStruct = sizeof(subject); subject.pcwszFilePath = path.c_str(); subject.hFile = file.handle;
    WINTRUST_DATA trust{};
    trust.cbStruct = sizeof(trust); trust.dwUIChoice = WTD_UI_NONE;
    trust.dwUnionChoice = WTD_CHOICE_FILE; trust.pFile = &subject;
    trust.dwStateAction = WTD_STATEACTION_VERIFY;
    // Verify Authenticode locally, without doing network work on a loader path.
    // Timestamped signatures retain the normal Windows trust semantics.
    trust.dwProvFlags = WTD_CACHE_ONLY_URL_RETRIEVAL | WTD_REVOCATION_CHECK_NONE;
    GUID action = WINTRUST_ACTION_GENERIC_VERIFY_V2;
    const LONG result = WinVerifyTrust(reinterpret_cast<HWND>(INVALID_HANDLE_VALUE), &action, &trust);
    bool nvidia = false;
    if (result == ERROR_SUCCESS)
    {
        auto* provider = WTHelperProvDataFromStateData(trust.hWVTStateData);
        auto* signer = provider ? WTHelperGetProvSignerFromChain(provider, 0, FALSE, 0) : nullptr;
        if (signer && signer->csCertChain && signer->pasCertChain && signer->pasCertChain[0].pCert)
        {
            const auto certificate = signer->pasCertChain[0].pCert;
            wchar_t commonName[256]{}, organization[256]{};
            nvidia = CertGetNameStringW(certificate, CERT_NAME_ATTR_TYPE, 0,
                const_cast<char*>(szOID_COMMON_NAME), commonName, static_cast<DWORD>(std::size(commonName))) > 1
                && CertGetNameStringW(certificate, CERT_NAME_ATTR_TYPE, 0,
                    const_cast<char*>(szOID_ORGANIZATION_NAME), organization, static_cast<DWORD>(std::size(organization))) > 1
                && wcscmp(commonName, L"NVIDIA Corporation") == 0 && wcscmp(organization, L"NVIDIA Corporation") == 0;
        }
    }
    trust.dwStateAction = WTD_STATEACTION_CLOSE;
    WinVerifyTrust(reinterpret_cast<HWND>(INVALID_HANDLE_VALUE), &action, &trust);
    return nvidia;
}
bool VersionDirectory(const std::wstring& name) noexcept
{
    return !name.empty() && name.size() <= 20
        && std::all_of(name.begin(), name.end(), [](wchar_t c) { return c >= L'0' && c <= L'9'; });
}
bool OtaLocation(const std::wstring& root, const std::wstring& path)
{
    const auto file = fs::absolute(path).lexically_normal();
    const auto files = file.parent_path();
    const auto version = files.parent_path();
    return _wcsicmp(file.filename().c_str(), L"160_E658700.bin") == 0
        && _wcsicmp(files.filename().c_str(), L"files") == 0 && VersionDirectory(version.filename().wstring())
        && _wcsicmp(version.parent_path().c_str(), fs::absolute(root).lexically_normal().c_str()) == 0;
}
bool InspectOtaLocked(const std::wstring& root, const std::wstring& path, File& file, Candidate& candidate)
{
    const auto canonical = fs::absolute(path).lexically_normal().wstring();
    return OtaLocation(root, canonical) && file.LockParents(fs::path(canonical).parent_path())
        && InspectLocked(canonical, file, candidate) && file.ExactPath(canonical);
}
Candidate NewestOta(const std::wstring& root)
{
    File directoryLease;
    if (!directoryLease.LockParents(fs::absolute(root).lexically_normal())) return {};
    std::vector<Candidate> eligible;
    std::error_code error;
    const ULONGLONG deadline = GetTickCount64() + 2000;
    for (fs::directory_iterator it(root, fs::directory_options::skip_permission_denied, error), end;
        !error && it != end; it.increment(error))
    {
        if (gExamined.fetch_add(1) >= 256 || GetTickCount64() >= deadline)
        { gScanLimited.store(true); break; }
        if (!VersionDirectory(it->path().filename().wstring())) continue;
        // Never recurse. The only candidate under each numeric version is this
        // exact file, and every ancestor must be a real directory, not a link.
        const auto path = (it->path() / L"files/160_E658700.bin").wstring();
        File file; Candidate candidate;
        if (InspectOtaLocked(root, path, file, candidate)
            && dlssg_provider_policy::IsSupportedAmpereVersion(candidate.version)) eligible.push_back(std::move(candidate));
    }
    if (error) gScanLimited.store(true);
    // If discovery was incomplete, the newest OTA version was not established.
    // Preserve the native game load instead of choosing a partial scan result.
    if (gScanLimited.load()) return {};
    std::sort(eligible.begin(), eligible.end(), [](const Candidate& a, const Candidate& b) {
        return Rank(a.version) != Rank(b.version) ? Rank(a.version) > Rank(b.version) : a.path < b.path;
    });
    for (const auto& selected : eligible)
    {
        if (GetTickCount64() >= deadline) { gScanLimited.store(true); return {}; }
        File file; Candidate current;
        if (!InspectOtaLocked(root, selected.path, file, current) || Rank(current.version) != Rank(selected.version)) continue;
        if (!NvidiaSignature(current.path, file)) { gSignatureRejections.fetch_add(1); continue; }
        current.nvidiaSigned = true;
        return current;
    }
    return {};
}
void PrepareRoot(const std::wstring& executableDirectory, const std::wstring& versionsDirectory) noexcept
{
    if (gPreparing.exchange(true, std::memory_order_acq_rel)) return;
    try
    {
        gExecutableDirectory.store(std::make_shared<const std::wstring>(executableDirectory), std::memory_order_release);
        gVersionsDirectory.store(std::make_shared<const std::wstring>(versionsDirectory), std::memory_order_release);
        gNativePinned.store(NativeProviderPinned());
        if (!gNativePinned.load())
            if (auto selected = NewestOta(versionsDirectory))
                gSelected.store(std::make_shared<const Candidate>(std::move(selected)), std::memory_order_release);
    }
    catch (...) { gScanLimited.store(true); }
    gPrepared.store(true, std::memory_order_release);
}
}

bool Inspect(const std::wstring& path, Candidate& candidate) noexcept
{
    try { File file; return InspectLocked(path, file, candidate); }
    catch (...) { candidate = {}; return false; }
}
bool InspectNvidiaSigned(const std::wstring& path, Candidate& candidate) noexcept
{
    try
    {
        File file;
        if (!InspectLocked(path, file, candidate) || !NvidiaSignature(candidate.path, file)) { candidate = {}; return false; }
        candidate.nvidiaSigned = true;
        return true;
    }
    catch (...) { candidate = {}; return false; }
}
void Prepare(const std::wstring& executableDirectory) noexcept
{ PrepareRoot(executableDirectory, kOtaVersionsDirectory); }
#if defined(MFG_PROVIDER_SELECTION_TESTING)
void PrepareForTests(const std::wstring& executableDirectory, const std::wstring& versionsDirectory) noexcept
{ PrepareRoot(executableDirectory, versionsDirectory); }
bool NvidiaSignatureForTests(const std::wstring& path) noexcept
{
    try { File file; return file.Open(path) && NvidiaSignature(fs::absolute(path).wstring(), file); }
    catch (...) { return false; }
}
#endif
LoadPlan Plan(const std::wstring& requested, HANDLE file, DWORD flags, bool beforeFirstCreate) noexcept
{
    if (!beforeFirstCreate || file || (flags & (DONT_RESOLVE_DLL_REFERENCES | LOAD_LIBRARY_AS_DATAFILE
        | LOAD_LIBRARY_AS_DATAFILE_EXCLUSIVE | LOAD_LIBRARY_AS_IMAGE_RESOURCE))) return {};
    const auto selected = gSelected.load(std::memory_order_acquire);
    const auto root = gVersionsDirectory.load(std::memory_order_acquire);
    if (!selected || !root || !gPrepared.load(std::memory_order_acquire) || gNativePinned.load()) return {};
    try
    {
        std::wstring request = requested;
        if (!fs::path(request).is_absolute())
            if (const auto directory = gExecutableDirectory.load()) request = (fs::path(*directory) / request).wstring();
        Candidate original;
        // Establish the requested feature before considering its version. Do
        // not search to resolve a native loader path we cannot identify here.
        if (!Inspect(request, original)) return {};
        auto lease = std::make_shared<File>(); Candidate current;
        if (!InspectOtaLocked(*root, selected->path, *lease, current) || Rank(current.version) != Rank(selected->version)
            || !dlssg_provider_policy::IsSupportedAmpereVersion(current.version) || !NvidiaSignature(current.path, *lease)) return {};
        gAttempts.fetch_add(1, std::memory_order_relaxed);
        return {current.path, std::move(lease), current.version, true};
    }
    catch (...) { return {}; }
}
bool Loaded(const LoadPlan& plan, HMODULE module) noexcept
{
    if (!plan) return false;
    wchar_t path[32768]{};
    const DWORD length = module ? GetModuleFileNameW(module, path, static_cast<DWORD>(std::size(path))) : 0;
    Version mapped{};
    const bool valid = plan.fileLease && plan.nvidiaSigned && length && length < std::size(path)
        && _wcsicmp(path, plan.path.c_str()) == 0 && dlssg_provider_policy::IsSupportedAmpereProvider(module, path)
        && MappedVersion(module, mapped) && Rank(mapped) == Rank(plan.version)
        && dlssg_provider_policy::IsSupportedAmpereVersion(mapped);
    (valid ? gLoaded : gFailures).fetch_add(1, std::memory_order_release);
    return valid;
}
Snapshot Read() noexcept
{
    Snapshot result{};
    try { if (const auto selected = gSelected.load(std::memory_order_acquire)) result.selected = *selected; }
    catch (...) {}
    result.prepared = gPrepared.load(std::memory_order_acquire); result.scanLimited = gScanLimited.load();
    result.nativeProviderPinned = gNativePinned.load(); result.examined = gExamined.load();
    result.signatureRejections = gSignatureRejections.load();
    result.attempts = gAttempts.load(); result.loaded = gLoaded.load(); result.failures = gFailures.load();
    return result;
}
}
