#include "dlssg_provider_selection.h"
#include <filesystem>
#include <fstream>
#include <iostream>
namespace select = dlssg_provider_selection;
namespace fs = std::filesystem;
unsigned checks = 0, failures = 0;
void Check(bool ok, const char* text)
{ ++checks; failures += !ok; std::cout << (ok ? "PASS " : "FAIL ") << text << '\n'; }
bool SameVersion(const select::Candidate& a, const select::Candidate& b)
{ return a && b && a.version.major == b.version.major && a.version.minor == b.version.minor && a.version.build == b.version.build; }
int wmain(int argc, wchar_t** argv)
{
    // mode, isolated versions root, real old game, real newer game, signed OTA,
    // real SR sibling, unsigned FG, unsupported FG, synthetic sibling, Microsoft PE
    if (argc != 11) return 2;
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    const std::wstring mode = argv[1];
    const fs::path root = fs::absolute(argv[2]).lexically_normal();
    const auto gameDirectory = root.parent_path() / L"native-game";
    select::Candidate old, newer, ota, candidate;
    Check(select::InspectNvidiaSigned(argv[3], old) && old.nvidiaSigned, "real older DLSS-G passes feature identity and NVIDIA Authenticode trust");
    Check(select::InspectNvidiaSigned(argv[4], newer) && newer.nvidiaSigned, "newer game DLSS-G is identified without entering OTA discovery");
    Check(select::InspectNvidiaSigned(argv[5], ota) && ota.nvidiaSigned, "real OTA .bin passes feature identity, version and NVIDIA signature");
    Check(!select::Inspect(argv[6], candidate) && !select::InspectNvidiaSigned(argv[6], candidate), "NVIDIA-signed SR sibling cannot establish DLSS-G identity");
    Check(select::Inspect(argv[7], candidate) && !select::InspectNvidiaSigned(argv[7], candidate), "unsigned supported FG is identifiable but cannot be an OTA candidate");
    Check(select::Inspect(argv[8], candidate) && !dlssg_provider_policy::IsSupportedVersion(candidate.version), "unsupported FG version is identifiable but ineligible");
    Check(!select::Inspect(argv[9], candidate), "DirectSR sibling export prevents FG identity despite matching version text");
    Check(!select::NvidiaSignatureForTests(argv[10]), "trusted Microsoft signature does not satisfy the NVIDIA signer rule");
    wchar_t eventName[128]{};
    swprintf_s(eventName, L"Local\\RTXMFG-Selection-%lu", GetCurrentProcessId());
    HANDLE event = CreateEventW(nullptr, TRUE, FALSE, eventName);
    SetEnvironmentVariableW(L"MFG_SELECTION_FIXTURE_EVENT", eventName);
    Check(select::Inspect(argv[7], candidate) && WaitForSingleObject(event, 0) == WAIT_TIMEOUT,
        "inspection never executes the unsigned fixture's DLL entry point");
    // The removed arbitrary path override must have no effect on selection.
    SetEnvironmentVariableW(L"RTX_MFG_DLSSG_PROVIDER_PATH", argv[4]);
    SetEnvironmentVariableW(L"RTXMFG_DIAGNOSTIC_NATIVE_DLSSG", mode == L"pinned" ? L"1" : nullptr);
    if (mode == L"auto") select::Prepare(fs::path(argv[3]).parent_path().wstring());
    else select::PrepareForTests(gameDirectory.wstring(), root.wstring());
    auto state = select::Read();
    Check(state.prepared, "provider preparation completes before first feature create");
    Check(state.nativeProviderPinned == (mode == L"pinned"), "status exposes exact-provider diagnostic mode separately from normal OTA selection");
    std::wcout << L"Selected " << state.selected.path << L" signed=" << state.selected.nvidiaSigned
        << L" examined=" << state.examined << L" signatureRejections=" << state.signatureRejections << L'\n';
    const bool positive = mode == L"strict" || mode == L"auto" || mode == L"changed" || mode == L"disappear";
    if (!positive)
    {
        Check(!state.selected, "no eligible signed OTA keeps the game's native provider load");
        Check(!select::Plan(argv[3], nullptr, 0, true) && !select::Plan(argv[4], nullptr, 0, true),
            "native fallback never searches another game or honors the removed arbitrary provider override");
        if (mode == L"unsigned") Check(state.signatureRejections > 0, "unsigned eligible OTA is rejected by trust verification");
        if (mode == L"pinned") Check(state.examined == 0, "exact-provider diagnostics perform no automatic provider search");
        if (mode == L"ineligible") Check(state.signatureRejections == 0, "sibling and unsupported identity are excluded before signature selection");
        if (mode == L"bounded") Check(state.scanLimited, "incomplete OTA inventory falls back natively instead of claiming a partial result is newest");
    }
    else
    {
        Check(SameVersion(state.selected, ota) && state.selected.nvidiaSigned, "newest eligible NVIDIA-signed OTA wins over higher unsigned and out-of-tree game copies");
        Check(!select::Plan(argv[3], nullptr, 0, false), "selection stays frozen after the first feature create");
        Check(!select::Plan(argv[3], reinterpret_cast<HANDLE>(1), 0, true), "loader file handles keep the original native call");
        for (DWORD flags : {DONT_RESOLVE_DLL_REFERENCES, LOAD_LIBRARY_AS_DATAFILE,
            LOAD_LIBRARY_AS_DATAFILE_EXCLUSIVE, LOAD_LIBRARY_AS_IMAGE_RESOURCE})
            Check(!select::Plan(argv[3], nullptr, flags, true), "resource-only loads keep their native loader arguments");
        Check(!select::Plan(argv[6], nullptr, 0, true) && !select::Plan(argv[9], nullptr, 0, true), "sibling loads cannot trigger OTA replacement");
        auto plan = select::Plan(argv[3], nullptr, 0, true);
        Check(plan && plan.nvidiaSigned && _wcsicmp(plan.path.c_str(), state.selected.path.c_str()) == 0, "requested older game FG redirects only to the chosen signed OTA file");
        Check(static_cast<bool>(select::Plan(argv[4], nullptr, 0, true)), "OTA-first also applies when the game's bundled version is higher");
        Check(!select::Plan(L"unresolved-native-provider.dll", nullptr, 0, true), "unresolved native search paths are passed through without a speculative replacement");
        if (mode == L"strict")
        {
            Check(static_cast<bool>(select::Plan(L"nvngx_dlssg.dll", nullptr, 0, true)), "identified relative native FG request can use the signed OTA file");
            Check(state.signatureRejections > 0, "higher unsigned OTA does not hide the next signed eligible version");
        }
        HANDLE writer = plan ? CreateFileW(plan.path.c_str(), GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr) : INVALID_HANDLE_VALUE;
        Check(plan && writer == INVALID_HANDLE_VALUE && GetLastError() == ERROR_SHARING_VIOLATION,
            "selected file is leased against replacement while the loader maps it");
        if (writer != INVALID_HANDLE_VALUE) CloseHandle(writer);
        Check(!select::Loaded(plan, nullptr), "failed OTA mapping is recorded without claiming provider load success");
        HMODULE module = plan ? LoadLibraryExW(plan.path.c_str(), nullptr, DONT_RESOLVE_DLL_REFERENCES) : nullptr;
        Check(module && select::Loaded(plan, module), "real OTA .bin maps and proves the selected owner's embedded version without NGX startup");
        auto mismatched = plan; mismatched.version = old.version;
        Check(!select::Loaded(mismatched, module), "same path cannot satisfy a mismatched mapped version");
        auto unsignedPlan = plan; unsignedPlan.nvidiaSigned = false;
        Check(!select::Loaded(unsignedPlan, module), "mapped owner requires a signed and leased selection plan");
        HMODULE wrong = LoadLibraryExW(argv[3], nullptr, DONT_RESOLVE_DLL_REFERENCES);
        Check(wrong && !select::Loaded(plan, wrong), "another mapped provider cannot satisfy chosen OTA provenance");
        if (wrong) FreeLibrary(wrong);
        if (module) FreeLibrary(module);
        mismatched = {}; unsignedPlan = {}; plan = {};
        Check(select::Read().loaded == 1 && select::Read().failures == 4, "status keeps verified and failed loads distinct");
        if (mode == L"changed" || mode == L"disappear")
        {
            // Only mutate an isolated fixture, never the real NVIDIA OTA root.
            const fs::path selected = fs::path(state.selected.path).lexically_normal();
            Check(selected.parent_path().parent_path().parent_path() == root, "mutation target is inside the explicit isolated fixture root");
            if (selected.parent_path().parent_path().parent_path() != root) return 2;
            if (mode == L"changed")
            {
                std::fstream file(selected, std::ios::in | std::ios::out | std::ios::binary);
                char byte{}; file.seekg(0x42); file.read(&byte, 1); byte ^= 1;
                file.seekp(0x42); file.write(&byte, 1); file.close();
                Check(select::Inspect(selected.wstring(), candidate) && SameVersion(candidate, ota), "tampered fixture retains the same FG identity and version");
                Check(!select::InspectNvidiaSigned(selected.wstring(), candidate), "changed signed bytes fail NVIDIA Authenticode verification");
            }
            else fs::rename(selected, selected.wstring() + L".removed");
            Check(!select::Plan(argv[3], nullptr, 0, true), "changed or missing selected OTA falls back to the original native load");
        }
    }
    SetEnvironmentVariableW(L"MFG_SELECTION_FIXTURE_EVENT", nullptr);
    CloseHandle(event);
    std::cout << checks << " checks, " << failures << " failures\n";
    return failures ? 1 : 0;
}
