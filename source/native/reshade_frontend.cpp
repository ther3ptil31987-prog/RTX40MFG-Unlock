#include "build_variant.h"
#include "unified_control_paths.h"
#include "status_transport.h"
#include "ampere_policy.h"
#include "nvidia_mfg_policy.h"
#include "ui_dynamic_mfg.h"
#include "universal_route_policy.h"
#include "reshade_frontend.h"

#include "backend_bridge.h"
#include "ampere_diagnostics.h"
#include "reflex_control.h"
#include "vsync_control.h"
#include "dlss_sr_policy.h"
#include "dlss5_settings.h"
#include "dlss5_auto.h"
#include "physical_key.h"

#include <imgui.h>
#include <imgui_internal.h>
#include "ui_host.h"
#include "standalone_ui.h"
#include "localization.h"
#include "single_module.h"
#include "witcher_dots/witcher_dots.h"
#include "gpu_name_override.h"

#include <Psapi.h>
#include <d3d12.h>
#include <dxgi.h>

#include <algorithm>
#include <charconv>
#include <cmath>
#include "ui_status_json.h"
#include "xess_ui_state.h"
#include "xess/runtime_search.h"
#include <atomic>
#include <cctype>
#include <cstdarg>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iterator>
#include <string>

namespace
{
using l10n::T;
constexpr const char* kConfigSection = MFG_PRODUCT;
constexpr const char* kOverlayTitle = MFG_OVERLAY_TITLE;
constexpr const char* kReShadeHomeWindow = "###home";

std::atomic<uint32_t> gGpuFamily{0};
bool UiAmpere() noexcept { return (gGpuFamily.load(std::memory_order_acquire) == 2u || gGpuFamily.load(std::memory_order_acquire) == 4u); }

std::atomic<bool> gRegistered{false};
std::atomic<bool> gInitialized{false};
HMODULE gSelf = nullptr;
HMODULE gReShade = nullptr;
bool gDefaultDockInitialized = false;
bool gOverlayVisitedThisFrame = false;
bool gOverlayDockedThisFrame = false;
enum class BackendKind : uint32_t
{
    eNone = 0,
    eNativeFile = 2,
};

HMODULE gBackend = nullptr;
MfgUnlockBackendInterface gBackendInterface{};
std::atomic<bool> gBackendConnected{false};
std::atomic<BackendKind> gBackendKind{BackendKind::eNone};
SRWLOCK gBackendConnectionLock = SRWLOCK_INIT;
std::wstring gNativeConfigPath;
std::wstring gNativeStatusPath;
// Display-only state. Control writes always obtain their own current snapshot.
MfgUnlockReShadeSnapshot gLastNativeDisplay{};
std::wstring gLastNativeDisplayPath;
bool gHaveNativeDisplay = false;
using MfgUnlockSampleFrameTelemetryFn = BOOL (WINAPI*)();
MfgUnlockSampleFrameTelemetryFn gSampleFrameTelemetry = nullptr;
std::atomic<uintptr_t> gTelemetrySwapchain{0};
std::atomic<uint64_t> gTelemetrySwapchainTick{0};

bool ConnectBackend() noexcept;
SRWLOCK gPresentationQueueLock = SRWLOCK_INIT;
ID3D12CommandQueue* gPresentationQueue = nullptr;

int gMultiplier = 2;
bool gFollowGameMode = true;
bool gDynamicMode = false;
int gDynamicTargetFrameRate = 0;
bool gDynamicLockToRefreshRate = true;
int gDynamicCustomTargetFrameRate = 60;
int gDlssgPreset = 2;
// DLSS Super Resolution preset (V1.6.0): 0 game/driver, 10..13 J..M.
uint32_t gDlssSrPreset = 0;
// DLSS 5 tab (V1.7.0): the edited settings; sliders save while dragging at
// most every 150 ms and once on release.
dlss5::Settings gDlss5{};
bool gDlss5SaveFailed=false,gDlss5Dirty=false;
uint64_t gDlss5LastSave=0;
bool gSrPresetSaveFailed = false;
// DLSS Ray Reconstruction preset: 0 game/driver, 4..6 D..F.
uint32_t gDlssRrPreset = 0;
bool gRrPresetSaveFailed = false;
int gVsyncMode = 0;
int gReflexFrameLimitFps = 0;
int gReflexCustomLimitFps = 120;
bool gPresentationSaveFailed = false;
bool gPresetSaveFailed = false;
bool gDynamicExperimental56 = false;
bool gGeneratedOnlyDebug = false;
bool gLastApplyAttempted = false;
bool gLastApplyAccepted = false;
bool gLastNativeConfigPersisted = true;


BOOL BackendGetSnapshot(MfgUnlockReShadeSnapshot* snapshot) noexcept
{
    if (!ConnectBackend() || !gBackendInterface.getSnapshot
        || !gBackendInterface.getSnapshot(snapshot))
    {
        return FALSE;
    }
    return TRUE;
}

BOOL BackendApplyControl(uint32_t multiplier, BOOL dynamicMode,
    uint32_t dynamicTargetFrameRate, BOOL dynamicExperimental56,
    BOOL generatedOnlyDebug) noexcept
{
    return ConnectBackend() && gBackendInterface.applyControl
        ? gBackendInterface.applyControl(multiplier, dynamicMode,
            dynamicTargetFrameRate, dynamicExperimental56,
            generatedOnlyDebug) : FALSE;
}

BOOL BackendRegisterDevice(ID3D12Device* device) noexcept
{
    return ConnectBackend() && gBackendInterface.registerD3D12Device
        ? gBackendInterface.registerD3D12Device(device) : FALSE;
}

BOOL BackendRegisterQueue(ID3D12CommandQueue* queue) noexcept
{
    return ConnectBackend() && gBackendInterface.registerD3D12Queue
        ? gBackendInterface.registerD3D12Queue(queue) : FALSE;
}

BOOL BackendRegisterSwapchain(IDXGISwapChain* swapchain,
    IUnknown* presentationQueue) noexcept
{
    return ConnectBackend() && gBackendInterface.registerD3D12Swapchain
        ? gBackendInterface.registerD3D12Swapchain(
            swapchain, presentationQueue) : FALSE;
}

BOOL BackendUnregisterSwapchain(IDXGISwapChain* swapchain) noexcept
{
    return ConnectBackend() && gBackendInterface.unregisterD3D12Swapchain
        ? gBackendInterface.unregisterD3D12Swapchain(swapchain) : FALSE;
}

bool BackendUsesNativeFile() noexcept
{
    return gBackendKind.load(std::memory_order_acquire)
        == BackendKind::eNativeFile;
}

bool FindJsonValue(const std::string& content, const char* name,
    size_t& offset) noexcept
{
    const std::string key = std::string("\"") + name + "\"";
    const size_t keyOffset = content.find(key);
    if (keyOffset == std::string::npos)
        return false;
    const size_t colon = content.find(':', keyOffset + key.size());
    if (colon == std::string::npos)
        return false;
    offset = content.find_first_not_of(" \t\r\n", colon + 1);
    return offset != std::string::npos;
}

bool ParseJsonBool(const std::string& content, const char* name,
    BOOL& value) noexcept
{
    size_t offset = 0;
    if (!FindJsonValue(content, name, offset))
        return false;
    if (content.compare(offset, 4, "true") == 0
        && ui_status_json::ValueEnd(content, offset+4))
    {
        value = TRUE;
        return true;
    }
    if (content.compare(offset, 5, "false") == 0
        && ui_status_json::ValueEnd(content, offset+5))
    {
        value = FALSE;
        return true;
    }
    return false;
}

template <typename T>
bool ParseJsonInteger(const std::string& content, const char* name,
    T& value) noexcept
{
    size_t offset = 0;
    if (!FindJsonValue(content, name, offset))
        return false;
    T parsed{};
    const auto result = std::from_chars(content.data()+offset,
        content.data()+content.size(), parsed);
    if (result.ec != std::errc{} || !ui_status_json::ValueEnd(content,
            static_cast<size_t>(result.ptr-content.data())))
        return false;
    value = parsed;
    return true;
}

bool ParseJsonFloat(const std::string& content, const char* name, float& value) noexcept
{
    size_t offset = 0;
    if (!FindJsonValue(content, name, offset)) return false;
    float parsed = 0.0f;
    const auto result = std::from_chars(content.data()+offset,
        content.data()+content.size(), parsed);
    if (result.ec != std::errc{} || !std::isfinite(parsed)
        || !ui_status_json::ValueEnd(content,
            static_cast<size_t>(result.ptr-content.data()))) return false;
    value = parsed;
    return true;
}

bool ParseJsonString(const std::string& content, const char* name,
    std::string& value) noexcept
{
    size_t offset = 0;
    if (!FindJsonValue(content, name, offset)
        || content[offset] != '"')
    {
        return false;
    }
    value.clear();
    for (size_t index = offset + 1; index < content.size(); ++index)
    {
        const char current = content[index];
        if (current == '"')
            return true;
        if (current != '\\')
        {
            value.push_back(current);
            continue;
        }
        if (++index >= content.size())
            return false;
        switch (content[index])
        {
        case '"': value.push_back('"'); break;
        case '\\': value.push_back('\\'); break;
        case '/': value.push_back('/'); break;
        case 'b': value.push_back('\b'); break;
        case 'f': value.push_back('\f'); break;
        case 'n': value.push_back('\n'); break;
        case 'r': value.push_back('\r'); break;
        case 't': value.push_back('\t'); break;
        default: return false;
        }
    }
    return false;
}

bool ReadTextFile(const std::wstring& path, std::string& content) noexcept
{
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return false;
    LARGE_INTEGER size{};
    const bool validSize = GetFileSizeEx(file, &size)
        && size.QuadPart > 0 && size.QuadPart <= 1024 * 1024;
    if (!validSize)
    {
        CloseHandle(file);
        return false;
    }
    content.resize(static_cast<size_t>(size.QuadPart));
    DWORD bytesRead = 0;
    const BOOL read = ReadFile(file, content.data(),
        static_cast<DWORD>(content.size()), &bytesRead, nullptr);
    CloseHandle(file);
    if (!read || bytesRead != content.size())
    {
        content.clear();
        return false;
    }
    return true;
}

bool ResolveNativeFilePaths() noexcept
{
    std::wstring executablePath(32768, L'\0');
    const DWORD length = GetModuleFileNameW(nullptr, executablePath.data(),
        static_cast<DWORD>(executablePath.size()));
    if (length == 0 || length >= executablePath.size())
        return false;
    executablePath.resize(length);
    const std::filesystem::path directory =
        std::filesystem::path(executablePath).parent_path();
    gNativeConfigPath = unified_control_paths::Config(directory.wstring());
    gNativeStatusPath = unified_control_paths::Status(gNativeConfigPath, directory.wstring());
    return true;
}

BOOL ParseNativeControlSnapshot(const std::string& control,
    MfgUnlockReShadeSnapshot* snapshot)
{
    if (!snapshot || snapshot->structSize < sizeof(*snapshot))
        return FALSE;
    MfgUnlockReShadeSnapshot output{};
    output.followGameMode = FALSE;
    output.intervalLoggingEnabled = TRUE;
    output.frontendClients = 1;
    strcpy_s(output.patchRoute, "pending");

    if (!ui_status_json::CompleteObject(control)
        || !ParseJsonInteger(control, "multiplier", output.desiredMultiplier)
        || output.desiredMultiplier < 1 || output.desiredMultiplier > 6) return FALSE;
    {
        size_t offset = 0;
        if (FindJsonValue(control, "followGame", offset)
            && !ParseJsonBool(control, "followGame", output.followGameMode)) return FALSE;
        if (FindJsonValue(control, "dynamicTargetFrameRate", offset)
            && (!ParseJsonInteger(control, "dynamicTargetFrameRate", output.dynamicTargetFrameRate)
                || output.dynamicTargetFrameRate > 1000)) return FALSE;
        if (FindJsonValue(control, "dlssgPreset", offset)
            && (!ParseJsonInteger(control, "dlssgPreset", output.dlssgPresetRequested)
                || output.dlssgPresetRequested > 3)) return FALSE;
        // An SR preset this build does not offer reads as game/driver; it
        // never invalidates the other saved options.
        if (FindJsonValue(control, "dlssSrPreset", offset)
            && (!ParseJsonInteger(control, "dlssSrPreset", output.dlssSr.presetRequested)
                || !dlss_sr::IsValidSelection(output.dlssSr.presetRequested)))
            output.dlssSr.presetRequested = dlss_sr::kGameOrDriver;
        if (FindJsonValue(control, "dlssRrPreset", offset)
            && (!ParseJsonInteger(control, "dlssRrPreset", output.dlssRr.presetRequested)
                || !dlss_sr::IsValidRrSelection(output.dlssRr.presetRequested)))
            output.dlssRr.presetRequested = dlss_sr::kGameOrDriver;
        if (!dlss5::settings_json::Parse(control, output.dlss5)) return FALSE;
        if (FindJsonValue(control, "vsyncMode", offset)
            && (!ParseJsonInteger(control, "vsyncMode", output.vsyncMode)
                || output.vsyncMode > 2)) return FALSE;
        if (FindJsonValue(control, "reflexFrameLimitFps", offset)
            && (!ParseJsonInteger(control, "reflexFrameLimitFps", output.reflexFrameLimitFps)
                || output.reflexFrameLimitFps > 1000)) return FALSE;
        // Legacy Dynamic 5X/6X preferences no longer control capacity.
        output.dynamicExperimental56 = FALSE;
        if (FindJsonValue(control, "generatedOnlyDebug", offset)
            && !ParseJsonBool(control, "generatedOnlyDebug", output.generatedOnlyDebug)) return FALSE;
        BOOL legacyValue = FALSE;
        for (const char* name : {"dynamicExperimental56", "selectiveOtaDlssgWrapper"})
            if (FindJsonValue(control, name, offset)
                && !ParseJsonBool(control, name, legacyValue)) return FALSE;
        if (FindJsonValue(control, "intervalLogging", offset)
            && !ParseJsonBool(control, "intervalLogging", output.intervalLoggingEnabled)) return FALSE;
        std::string controlMode;
        if (FindJsonValue(control, "mode", offset))
        {
            if (!ParseJsonString(control, "mode", controlMode)
                || (controlMode != "fixed" && controlMode != "dynamic"
                    && !(controlMode == "follow" && output.followGameMode))) return FALSE;
            output.dynamicMode = controlMode == "dynamic" ? TRUE : FALSE;
        }
        if (output.followGameMode) output.dynamicMode = FALSE;
    }

    *snapshot = output;
    return TRUE;
}

BOOL ReadNativeControlSnapshot(MfgUnlockReShadeSnapshot* snapshot)
{
    std::string control;
    return ReadTextFile(gNativeConfigPath, control)
        && ParseNativeControlSnapshot(control, snapshot);
}

uint64_t NativeStatusUnixSeconds() noexcept
{
    FILETIME time{};
    GetSystemTimeAsFileTime(&time);
    const uint64_t ticks = (uint64_t(time.dwHighDateTime) << 32) | time.dwLowDateTime;
    return ticks/10000000ull - 11644473600ull;
}

bool ValidNativeStatus(const std::string& status)
{
    if (!ui_status_json::CompleteObject(status)) return false;
    uint32_t version = 0, pid = 0, maximum = 0;
    uint64_t heartbeat = 0, processBirth = 0;
    const uint64_t now = NativeStatusUnixSeconds();
    if (!ParseJsonInteger(status, "version", version)
        || version != MFG_STATUS_VERSION_NUMBER
        || !ParseJsonInteger(status, "pid", pid) || pid != GetCurrentProcessId()
        || !ParseJsonInteger(status, "processBirth", processBirth)
        || !processBirth || processBirth != diagnostic_paths::ProcessBirth()
        || !ParseJsonInteger(status, "heartbeat", heartbeat)
        || heartbeat > now || now-heartbeat > 5
        || !ParseJsonInteger(status, "safeMaximumMultiplier", maximum)
        || maximum < 2 || maximum > 6u) return false;
    // These fields define availability and the menu's shape. Never silently
    // substitute defaults for a missing or ill-typed capability field.
    BOOL value = FALSE;
    for (const char* key : {"bridgeReady", "gameFrameGenerationOn",
            "appliedFrameGenerationOn",
            "activeWrapperObserved", "dynamicMfgSupportKnown", "dynamicMfgSupported"})
        if (!ParseJsonBool(status, key, value)) return false;
    return true;
}

// XeSS-FG state lives beside the shared ReShade snapshot ABI: only the
// integrated menu draws it, so the bridge contract is unchanged.
using XessUiState = xess_ui::State;
XessUiState gXessUi;
// The frame generation the game last turned on; the menu keeps its settings
// while the game's frame generation is off.
std::string gLastGameFrameGeneration = "none";

void ParseXessUiState(const std::string& status) noexcept
{
    try
    {
        gXessUi = xess_ui::Read(status, &ParseJsonBool,
            [](const std::string& text, const char* key, auto& value) {
                return ParseJsonInteger(text, key, value);
            }, &ParseJsonString);
        if (gXessUi.activeFrameGeneration == "xess" || gXessUi.activeFrameGeneration == "dlss")
            gLastGameFrameGeneration = gXessUi.activeFrameGeneration;
    }
    catch (...) {}
}

// One DLSS feature's status keys, prefix "dlssSr" or "dlssRr". Saved intent
// outranks the status' requested preset.
void ParseDlssFeature(const std::string& status, const char* prefix, bool haveSavedControl,
    MfgUnlockDlssFeature& feature) noexcept
{
    char buffer[48]{};
    const auto key = [&](const char* name) { snprintf(buffer, sizeof(buffer), "%s%s", prefix, name); return buffer; };
    if (!haveSavedControl) ParseJsonInteger(status, key("PresetRequested"), feature.presetRequested);
    ParseJsonBool(status, key("Detected"), feature.detected);
    ParseJsonBool(status, key("Running"), feature.running);
    ParseJsonInteger(status, key("LiveFeatures"), feature.liveFeatures);
    ParseJsonInteger(status, key("RenderWidth"), feature.renderWidth);
    ParseJsonInteger(status, key("RenderHeight"), feature.renderHeight);
    ParseJsonInteger(status, key("OutputWidth"), feature.outputWidth);
    ParseJsonInteger(status, key("OutputHeight"), feature.outputHeight);
    ParseJsonInteger(status, key("Quality"), feature.quality);
    ParseJsonInteger(status, key("GameHint"), feature.gameHint);
    ParseJsonInteger(status, key("ActivePreset"), feature.activePreset);
    ParseJsonInteger(status, key("LiveSwitch"), feature.liveSwitch);
    ParseJsonBool(status, key("DrsObserved"), feature.drsObserved);
    ParseJsonBool(status, key("DriverPresetValid"), feature.driverPresetValid);
    ParseJsonInteger(status, key("DriverPreset"), feature.driverPreset);
    ParseJsonBool(status, key("DrsSuppressed"), feature.drsSuppressed);
    ParseJsonBool(status, key("FixedByDriver"), feature.fixedByDriver);
    ParseJsonBool(status, key("DriverOverrideCached"), feature.driverOverrideCached);
}

BOOL WINAPI NativeFileGetSnapshot(MfgUnlockReShadeSnapshot* snapshot)
{
    if (!snapshot || snapshot->structSize < sizeof(*snapshot)) return FALSE;
    MfgUnlockReShadeSnapshot output{};
    const bool haveSavedControl = ReadNativeControlSnapshot(&output) != FALSE;
    std::string status;
    const auto statusSource=status_transport::Read(gNativeStatusPath,status,&ReadTextFile,&ValidNativeStatus);
    if(statusSource==status_transport::Source::None)return FALSE;
    static auto previousSource=status_transport::Source::None;
    if(statusSource!=previousSource){
        single_module::Log(statusSource==status_transport::Source::Fallback
            ?L"MFG_STATUS_UI fresh snapshot source=process-memory"
            :L"MFG_STATUS_UI fresh snapshot source=config-directory");
        previousSource=statusSource;
    }
    {
        // A status heartbeat can precede the latest atomic config save. Saved
        // intent remains authoritative; applied state comes only from status.
        if (!haveSavedControl) ParseJsonBool(status, "followGame", output.followGameMode);
        ParseJsonInteger(status, "version",
            output.statusProtocolVersion);
        if (!ParseJsonInteger(status, "gpuFamily", output.gpuFamily) || output.gpuFamily > 5u)
            return FALSE;
        ParseJsonInteger(status, "gpuAdapterLuid", output.gpuAdapterLuid);
        ParseJsonInteger(status, "gpuSelectionFailure", output.gpuSelectionFailure);
        ParseJsonInteger(status, "graphicsApi", output.graphicsApi);
        gGpuFamily.store(output.gpuFamily, std::memory_order_release);
        ParseXessUiState(status);
        if (UiAmpere())
        {
            if (output.statusProtocolVersion != MFG_STATUS_VERSION_NUMBER) return FALSE;
            ParseJsonBool(status, "ampereProgramReadyMfg", output.ampereProgramReadyMfg);
            ParseJsonInteger(status, "ampereKernelImage", output.ampereKernelImage);
            ParseJsonInteger(status, "ampereNativeCacheStatus", output.ampereNativeCacheStatus);
            ParseJsonInteger(status, "ampereCertifiedMaximum", output.ampereCertifiedMaximum);
            ParseJsonInteger(status, "ampereFailure", output.ampereFailure);
            ParseJsonInteger(status, "presetQueries", output.presetQueries);
            ParseJsonInteger(status, "ampereFirstFailure", output.ampereFirstFailure);
            ParseJsonInteger(status, "ampereFirstNgxResult", output.ampereFirstNgxResult);
            ParseJsonInteger(status, "amperePrimaryFailure", output.amperePrimaryFailure);
            ParseJsonInteger(status, "amperePrimaryNgxResult", output.amperePrimaryNgxResult);
            ParseJsonInteger(status, "ampereLastFailure", output.ampereLastFailure);
            ParseJsonInteger(status, "ampereLastNgxResult", output.ampereLastNgxResult);
            ParseJsonInteger(status, "ampereCreateAttempts", output.ampereCreateAttempts);
            ParseJsonInteger(status, "ampereCreateBlockedBeforeProvider", output.ampereCreateBlockedBeforeProvider);
            ParseJsonInteger(status, "ampereEvaluateAttempts", output.ampereEvaluateAttempts);
            ParseJsonInteger(status, "amperePreparationStage", output.amperePreparationStage);
            ParseJsonInteger(status, "ampereStartupFailureMask", output.ampereStartupFailureMask);
            ParseJsonInteger(status, "ampereCandidateVersionMajor", output.ampereCandidateVersionMajor);
            ParseJsonInteger(status, "ampereCandidateVersionMinor", output.ampereCandidateVersionMinor);
            ParseJsonInteger(status, "ampereCandidateVersionBuild", output.ampereCandidateVersionBuild);
            ParseJsonInteger(status, "ampereCreatedFeatures", output.ampereCreatedFeatures);
            ParseJsonInteger(status, "ampereEvaluations", output.ampereEvaluations);
            if (!ParseJsonBool(status, "ampereLegacySinglePreset", output.ampereLegacySinglePreset))
                return FALSE;
        }
        ParseJsonBool(status, "bridgeReady", output.bridgeReady);
        ParseJsonBool(status, "loaderCoreImported",
            output.loaderCoreImported);
        ParseJsonBool(status, "mainResolverDiscoveryInstalled",
            output.mainResolverDiscoveryInstalled);
        ParseJsonBool(status, "slInitEntryDetourCurrent",
            output.slInitEntryDetourCurrent);
        ParseJsonBool(status, "slInitIatFallbackInstalled",
            output.slInitIatFallbackInstalled);
        ParseJsonBool(status, "slInitResolverFallbackActive",
            output.slInitResolverFallbackActive);
        ParseJsonBool(status, "slInitControlPathReady",
            output.slInitControlPathReady);
        ParseJsonInteger(status, "slInitCalls", output.slInitCalls);
        ParseJsonInteger(status, "slInitFlagsBefore",
            output.slInitFlagsBefore);
        ParseJsonInteger(status, "slInitFlagsAfter",
            output.slInitFlagsAfter);
        ParseJsonBool(status, "otaPreferencesForced",
            output.otaPreferencesForced);
        ParseJsonBool(status, "otaPreferencesEnabledAtInit",
            output.otaPreferencesEnabledAtInit);
        ParseJsonBool(status, "downloadedStreamlinePluginsEnabledAtInit",
            output.downloadedStreamlinePluginsEnabledAtInit);
        ParseJsonBool(status, "otaProviderPreflightSupported",
            output.otaProviderPreflightSupported);
        ParseJsonBool(status, "otaForceSuppressed",
            output.otaForceSuppressed);
        ParseJsonBool(status, "nvidiaCompatibilityResolved",
            output.nvidiaCompatibilityResolved);
        ParseJsonInteger(status, "nvidiaProfileStatus",
            output.nvidiaProfileStatus);
        std::string nvidiaProfileName;
        if (ParseJsonString(status, "nvidiaProfileName",
                nvidiaProfileName))
        {
            strncpy_s(output.nvidiaProfileName,
                nvidiaProfileName.c_str(), _TRUNCATE);
        }
        ParseJsonInteger(status, "nvidiaCompatibilityTier",
            output.nvidiaCompatibilityTier);
        ParseJsonInteger(status, "nvidiaCompatibilityManifestEntries",
            output.nvidiaCompatibilityManifestEntries);
        ParseJsonInteger(status, "nvidiaPolicyCeilingMultiplier",
            output.nvidiaPolicyCeilingMultiplier);
        ParseJsonInteger(status, "wrapperNativeMaximumMultiplier",
            output.wrapperNativeMaximumMultiplier);
        ParseJsonBool(status, "compatibilityFallback",
            output.compatibilityFallback);
        ParseJsonInteger(status, "compatibilityReason",
            output.compatibilityReason);
        ParseJsonBool(status, "fullStreamlineOtaRequested",
            output.fullStreamlineOtaRequested);
        ParseJsonBool(status, "fullStreamlineOtaEligible",
            output.fullStreamlineOtaEligible);
        ParseJsonBool(status, "downloadedStreamlinePluginsForced",
            output.downloadedStreamlinePluginsForced);
        ParseJsonInteger(status, "streamlineHostVersionMajor",
            output.streamlineHostVersionMajor);
        ParseJsonInteger(status, "streamlineHostVersionMinor",
            output.streamlineHostVersionMinor);
        ParseJsonInteger(status, "streamlineHostVersionBuild",
            output.streamlineHostVersionBuild);
        ParseJsonInteger(status, "streamlineHostVersionPrivate",
            output.streamlineHostVersionPrivate);
        ParseJsonBool(status, "streamlineLoaderDiscoveryInstalled",
            output.streamlineLoaderDiscoveryInstalled);
        ParseJsonInteger(status, "streamlineLoaderDiscoveryCalls",
            output.streamlineLoaderDiscoveryCalls);
        ParseJsonBool(status, "setOptionsEntryDetourCurrent",
            output.setOptionsEntryDetourCurrent);
        ParseJsonBool(status, "setOptionsResolverFallbackActive",
            output.setOptionsResolverFallbackActive);
        ParseJsonInteger(status, "setOptionsResolverFallbackCalls",
            output.setOptionsResolverFallbackCalls);
        ParseJsonBool(status, "setOptionsControlPathReady",
            output.setOptionsControlPathReady);
        ParseJsonBool(status, "getStateEntryDetourCurrent",
            output.getStateEntryDetourCurrent);
        ParseJsonBool(status, "ngxCreateEntryDetourCurrent",
            output.ngxCreateEntryDetourCurrent);
        ParseJsonBool(status, "ngxEvaluateEntryDetourCurrent",
            output.ngxEvaluateEntryDetourCurrent);
        ParseJsonBool(status, "backportReadyAtCreate",
            output.backportReadyAtCreate);
        ParseJsonBool(status, "pipelineMayPredateDetour",
            output.pipelineMayPredateDetour);
        ParseJsonBool(status, "gameFrameGenerationOn",
            output.gameFrameGenerationOn);
        ParseJsonBool(status, "appliedFrameGenerationOn",
            output.appliedFrameGenerationOn);
        ParseJsonBool(status, "streamlineRebuildRequired",
            output.streamlineRebuildRequired);
        ParseJsonBool(status, "perSampleSynthesisReady",
            output.perSampleSynthesisReady);
        ParseJsonBool(status, "highCapabilityPublicationAllowed",
            output.highCapabilityPublicationAllowed);
        ParseJsonBool(status, "dllNotificationRegistered",
            output.dllNotificationRegistered);
        ParseJsonBool(status, "streamlinePluginLoaderHooksInstalled",
            output.pluginLoaderHooksInstalled);
        ParseJsonBool(status, "activeWrapperObserved",
            output.activeWrapperObserved);
        ParseJsonBool(status, "activeWrapperUsesNvidiaOta",
            output.activeWrapperUsesNvidiaOta);
        ParseJsonInteger(status, "activeWrapperVersionMajor",
            output.activeWrapperVersionMajor);
        ParseJsonInteger(status, "activeWrapperVersionMinor",
            output.activeWrapperVersionMinor);
        ParseJsonInteger(status, "activeWrapperVersionBuild",
            output.activeWrapperVersionBuild);
        ParseJsonInteger(status, "activeWrapperVersionPrivate",
            output.activeWrapperVersionPrivate);
        ParseJsonInteger(status,
            "wrapperCompiledMaximumGeneratedFrames",
            output.wrapperCompiledMaximumGeneratedFrames);
        ParseJsonInteger(status, "safeMaximumMultiplier",
            output.safeMaximumMultiplier);
        if (!haveSavedControl)
            ParseJsonInteger(status, "dlssgPresetRequested", output.dlssgPresetRequested);
        ParseJsonInteger(status, "dlssgPresetLatched", output.dlssgPresetLatched);
        ParseJsonBool(status, "dlssgPresetSelectionFrozen", output.dlssgPresetSelectionFrozen);
        ParseJsonBool(status, "dlssgPresetRestartRequired", output.dlssgPresetRestartRequired);
        ParseJsonInteger(status, "dlssgPresetObserved", output.dlssgPresetObserved);
        ParseJsonBool(status, "dlssgPresetObservedValid", output.dlssgPresetObservedValid);
        ParseJsonInteger(status, "dlssgPresetOverrideReadCount", output.dlssgPresetOverrideReadCount);
        ParseJsonBool(status, "dlssgPresetOverrideInstalled", output.dlssgPresetOverrideInstalled);
        ParseJsonInteger(status, "dlssgPresetOverrideFailure", output.dlssgPresetOverrideFailure);
        ParseJsonInteger(status, "dlssgPresetReadCount", output.dlssgPresetReadCount);
        ParseJsonBool(status, "dlssgPresetCAvailable", output.dlssgPresetCAvailable);
        ParseJsonInteger(status, "dlssgPresetLiveSwitch", output.dlssgPresetLiveSwitch);
        ParseJsonBool(status, "dlssgPresetSwitching", output.dlssgPresetSwitching);
        ParseDlssFeature(status, "dlssSr", haveSavedControl, output.dlssSr);
        ParseDlssFeature(status, "dlssRr", haveSavedControl, output.dlssRr);
        if(!haveSavedControl)dlss5::settings_json::Parse(status,output.dlss5);
        ParseJsonBool(status,"dlss5Detected",output.dlss5Detected);
        ParseJsonBool(status,"dlss5Pending",output.dlss5Pending);
        ParseJsonInteger(status,"dlss5State",output.dlss5State);
        ParseJsonInteger(status,"dlss5Result",output.dlss5Result);
        ParseJsonInteger(status,"dlss5Route",output.dlss5Route);
        ParseJsonInteger(status,"dlss5Stage",output.dlss5Stage);
        ParseJsonInteger(status,"dlss5Passes",output.dlss5Passes);
        ParseJsonInteger(status,"dlss5Width",output.dlss5Width);
        ParseJsonInteger(status,"dlss5Height",output.dlss5Height);
        ParseJsonInteger(status,"dlss5ExtentWidth",output.dlss5ExtentWidth);
        ParseJsonFloat(status,"dlss5EffectiveScale",output.dlss5EffectiveScale);
        ParseJsonFloat(status,"dlss5GpuMs",output.dlss5GpuMs);
        ParseJsonFloat(status,"dlss5BudgetMs",output.dlss5BudgetMs);
        ParseJsonFloat(status,"dlss5ExposureGain",output.dlss5ExposureGain);
        ParseJsonFloat(status,"dlss5FrameMs",output.dlss5FrameMs);
        ParseJsonFloat(status,"dlss5BoostNetworkMs",output.dlss5BoostNetworkMs);
        ParseJsonBool(status,"dlss5ScaleFloorHeld",output.dlss5ScaleFloorHeld);
        std::string optiScalerUiOwner;
        if (ParseJsonString(status, "optiScalerUiOwner", optiScalerUiOwner))
            output.optiScalerUiOwner = optiScalerUiOwner == "shared" ? 1u : optiScalerUiOwner == "optiscaler" ? 2u
                : optiScalerUiOwner == "rtxmfg" ? 3u : 0u;
        ParseJsonInteger(status,"dlss5ExtentHeight",output.dlss5ExtentHeight);
        ParseJsonInteger(status,"dlss5Evaluations",output.dlss5Evaluations);
        ParseJsonBool(status, "requestedMultiplierLimited",
            output.requestedMultiplierLimited);
        if (!haveSavedControl) ParseJsonInteger(status, "multiplier", output.desiredMultiplier);
        ParseJsonInteger(status, "appliedMultiplier",
            output.appliedMultiplier);
        if (!haveSavedControl)
            ParseJsonInteger(status, "dynamicTargetFrameRate", output.dynamicTargetFrameRate);
        BOOL appliedTargetValid = FALSE;
        float appliedTarget = 0.0f;
        if (ParseJsonBool(status, "appliedDynamicTargetValid", appliedTargetValid)
            && appliedTargetValid
            && ParseJsonFloat(status, "appliedDynamicTargetFrameRate", appliedTarget)
            && appliedTarget >= 0.0f)
        {
            output.appliedDynamicTargetFrameRate = appliedTarget;
            output.appliedDynamicTargetValid = TRUE;
        }
        ParseJsonInteger(status, "requestRevision",
            output.desiredRevision);
        ParseJsonInteger(status, "appliedRevision",
            output.appliedRevision);
        ParseJsonInteger(status, "actualFramesPresented",
            output.actualFramesPresented);
        ParseJsonInteger(status, "numFramesToGenerateMax",
            output.numFramesToGenerateMax);
        ParseJsonInteger(status, "setOptionsResult",
            output.lastSetOptionsResult);
        ParseJsonInteger(status, "getStateResult",
            output.lastGetStateResult);
        ParseJsonBool(status, "setOptionsSeen",
            output.setOptionsSeen);
        ParseJsonBool(status, "setOptionsAccepted",
            output.setOptionsAccepted);
        ParseJsonBool(status, "getStateSeen",
            output.getStateSeen);
        ParseJsonBool(status, "dynamicMfgSupportKnown",
            output.dynamicMfgSupportKnown);
        ParseJsonBool(status, "dynamicMfgSupported",
            output.dynamicMfgSupported);
        ParseJsonBool(status, "fgVsyncSupportKnown", output.fgVsyncSupportKnown);
        ParseJsonBool(status, "fgVsyncSupported", output.fgVsyncSupported);
        if (!haveSavedControl)
        {
            ParseJsonInteger(status, "vsyncMode", output.vsyncMode);
            ParseJsonInteger(status, "reflexFrameLimitFps", output.reflexFrameLimitFps);
        }
        ParseJsonBool(status, "dynamicVsyncAvailable", output.dynamicVsyncAvailable);
        ParseJsonBool(status, "vsyncControlAvailable", output.vsyncControlAvailable);
        ParseJsonBool(status, "vsyncOverrideApplied", output.vsyncOverrideApplied);
        ParseJsonBool(status, "vsyncPresentationObserved", output.vsyncPresentationObserved);
        ParseJsonInteger(status, "vsyncOriginalInterval", output.vsyncOriginalInterval);
        ParseJsonInteger(status, "vsyncSubmittedInterval", output.vsyncSubmittedInterval);
        ParseJsonInteger(status, "vsyncFailure", output.vsyncFailure);
        ParseJsonBool(status, "vsyncVulkanSwapchain", output.vsyncVulkanSwapchain);
        ParseJsonBool(status, "vsyncVulkanHook", output.vsyncVulkanHook);
        ParseJsonInteger(status, "vsyncOriginalPresentMode", output.vsyncOriginalPresentMode);
        ParseJsonInteger(status, "vsyncSubmittedPresentMode", output.vsyncSubmittedPresentMode);
        ParseJsonBool(status, "reflexControlAvailable", output.reflexControlAvailable);
        ParseJsonBool(status, "reflexAppliedKnown", output.reflexAppliedKnown);
        ParseJsonInteger(status, "reflexAppliedFrameLimitUs", output.reflexAppliedFrameLimitUs);
        ParseJsonBool(status, "reflexLimitPending", output.reflexLimitPending);
        ParseJsonBool(status, "reflexRestorePending", output.reflexRestorePending);
        ParseJsonInteger(status, "reflexStatus", output.reflexStatus);
        ParseJsonInteger(status, "reflexLastResult", output.reflexLastResult);
        ParseJsonInteger(status, "reflexHookMask", output.reflexHookMask);
        ParseJsonInteger(status, "reflexModuleVersionMajor", output.reflexModuleVersionMajor);
        ParseJsonInteger(status, "reflexModuleVersionMinor", output.reflexModuleVersionMinor);
        ParseJsonInteger(status, "reflexModuleVersionPatch", output.reflexModuleVersionPatch);
        ParseJsonInteger(status, "reflexModuleGeneration", output.reflexModuleGeneration);
        ParseJsonBool(status, "intervalLoggingEnabled",
            output.intervalLoggingEnabled);
        ParseJsonBool(status, "intervalLogReady",
            output.intervalLogReady);
        ParseJsonInteger(status, "intervalValidSamples",
            output.intervalValidSamples);
        ParseJsonInteger(status, "intervalInvalidSamples",
            output.intervalInvalidSamples);
        ParseJsonInteger(status, "intervalDroppedSamples",
            output.intervalDroppedSamples);
        ParseJsonInteger(status, "intervalLastCount",
            output.intervalLastCount);
        ParseJsonInteger(status, "intervalLastIndex",
            output.intervalLastIndex);
        ParseJsonInteger(status, "intervalLastPositionNumerator",
            output.intervalLastPositionNumerator);
        ParseJsonInteger(status, "intervalLastPositionDenominator",
            output.intervalLastPositionDenominator);
        ParseJsonInteger(status, "realFpsMilli", output.realFpsMilli);
        ParseJsonInteger(status, "dlssFpsMilli", output.dlssFpsMilli);
        ParseJsonInteger(status, "fpsOutputSource", output.fpsOutputSource);
        ParseJsonInteger(status, "fpsSampleAgeMs", output.fpsSampleAgeMs);
        ParseJsonBool(status, "uiInputsReady", output.uiInputsReady);
        ParseJsonBool(status, "uiRecompositionEnabled",
            output.uiRecompositionEnabled);
        ParseJsonBool(status, "uiRecompositionForced",
            output.uiRecompositionForced);
        ParseJsonBool(status, "gameUiRecompositionEnabled", output.gameUiRecompositionEnabled);
        if (!haveSavedControl)
            ParseJsonBool(status, "generatedOnlyDebug", output.generatedOnlyDebug);
        ParseJsonBool(status, "appliedGeneratedOnlyDebug",
            output.appliedGeneratedOnlyDebug);
        std::string intervalFile;
        if (ParseJsonString(status, "intervalLogFile", intervalFile))
            strncpy_s(output.intervalLogFile,
                intervalFile.c_str(), _TRUNCATE);
        std::string mode;
        if (!haveSavedControl && ParseJsonString(status, "mode", mode))
            output.dynamicMode = mode == "dynamic" ? TRUE : FALSE;
        std::string appliedMode;
        if (ParseJsonString(status, "appliedMode", appliedMode))
        {
            output.appliedDynamicMode =
                appliedMode == "dynamic" ? TRUE : FALSE;
        }
        std::string route;
        if (ParseJsonString(status, "route", route))
            strncpy_s(output.patchRoute, route.c_str(), _TRUNCATE);
        std::string activeWrapperPath;
        if (ParseJsonString(status, "activeWrapperPath", activeWrapperPath))
            strncpy_s(output.activeWrapperPath,
                activeWrapperPath.c_str(), _TRUNCATE);
        ParseJsonInteger(status, "activeWrapperGeneration",
            output.activeWrapperGeneration);
        std::string activeControlPath;
        if (ParseJsonString(status, "activeControlPath", activeControlPath))
            strncpy_s(output.activeControlPath,
                activeControlPath.c_str(), _TRUNCATE);
        std::string activeStatePath;
        if (ParseJsonString(status, "activeStatePath", activeStatePath))
            strncpy_s(output.activeStatePath,
                activeStatePath.c_str(), _TRUNCATE);
        std::string activeControlDetour;
        if (ParseJsonString(status, "activeControlDetour",
                activeControlDetour))
            strncpy_s(output.activeControlDetour,
                activeControlDetour.c_str(), _TRUNCATE);
        std::string activeStateDetour;
        if (ParseJsonString(status, "activeStateDetour", activeStateDetour))
            strncpy_s(output.activeStateDetour,
                activeStateDetour.c_str(), _TRUNCATE);
        std::string activeProviderPath;
        if (ParseJsonString(status, "activeProviderPath", activeProviderPath))
            strncpy_s(output.activeProviderPath,
                activeProviderPath.c_str(), _TRUNCATE);
        ParseJsonInteger(status, "activeProviderVersionMajor",
            output.activeProviderVersionMajor);
        ParseJsonInteger(status, "activeProviderVersionMinor",
            output.activeProviderVersionMinor);
        ParseJsonInteger(status, "activeProviderVersionBuild",
            output.activeProviderVersionBuild);
        ParseJsonInteger(status, "activeProviderVersionPrivate",
            output.activeProviderVersionPrivate);
        ParseJsonInteger(status, "activeProviderGeneration",
            output.activeProviderGeneration);
        std::string providerSelectionSource;
        if (ParseJsonString(status, "providerSelectionSource",
                providerSelectionSource))
            strncpy_s(output.providerSelectionSource,
                providerSelectionSource.c_str(), _TRUNCATE);
        std::string providerCreateDetour;
        if (ParseJsonString(status, "providerCreateDetour",
                providerCreateDetour))
            strncpy_s(output.providerCreateDetour,
                providerCreateDetour.c_str(), _TRUNCATE);
        std::string providerEvaluateDetour;
        if (ParseJsonString(status, "providerEvaluateDetour",
                providerEvaluateDetour))
            strncpy_s(output.providerEvaluateDetour,
                providerEvaluateDetour.c_str(), _TRUNCATE);
        ParseJsonBool(status, "midpointReadyAtFirstCreate",
            output.midpointReadyAtFirstCreate);
        ParseJsonInteger(status, "activeLastCallRevision",
            output.activeLastCallRevision);
        ParseJsonInteger(status, "activeLastAcceptedRevision",
            output.activeLastAcceptedRevision);
        ParseJsonInteger(status, "universalRouteFailure",
            output.universalRouteFailure);
        std::string routeFailureReason;
        if (ParseJsonString(status, "universalRouteFailureReason",
                routeFailureReason))
            strncpy_s(output.universalRouteFailureReason,
                routeFailureReason.c_str(), _TRUNCATE);
        ParseJsonBool(status, "releaseEntryCurrent",
            output.releaseEntryCurrent);
        ParseJsonBool(status, "frameGenerationOffAccepted",
            output.frameGenerationOffAccepted);
        ParseJsonBool(status, "releaseObserved", output.releaseObserved);
        output.perSampleSynthesisReady = output.midpointReadyAtFirstCreate;
        output.highCapabilityPublicationAllowed =
            output.bridgeReady && output.midpointReadyAtFirstCreate;
    }
    *snapshot = output;
    return TRUE;
}

std::string NativeControlJson(uint32_t multiplier, BOOL followGame, BOOL dynamicMode,
    uint32_t dynamicTargetFrameRate, BOOL generatedOnlyDebug, BOOL intervalLogging,
    uint32_t preset, uint32_t vsyncMode = 0, uint32_t reflexFrameLimitFps = 0)
{
    char content[384]{};
    const int length = std::snprintf(content, sizeof(content),
        "{\"followGame\":%s,\"mode\":\"%s\",\"multiplier\":%u,"
        "\"dynamicTargetFrameRate\":%u,\"dlssgPreset\":%u,"
        "\"vsyncMode\":%u,\"reflexFrameLimitFps\":%u,"
        "\"generatedOnlyDebug\":%s,\"intervalLogging\":%s,"
        "\"version\":13}\n",
        followGame ? "true" : "false",
        followGame ? "follow"
            : dynamicMode ? "dynamic" : "fixed",
        multiplier,
        dynamicTargetFrameRate,
        preset, vsyncMode, reflexFrameLimitFps,
        generatedOnlyDebug ? "true" : "false", intervalLogging ? "true" : "false");
    if (length <= 0 || static_cast<size_t>(length) >= sizeof(content))
        return {};
    return std::string(content, static_cast<size_t>(length));
}

// Carries a raw JSON value of name from the saved control into content when the
// new content lacks it. Writers that rebuild the whole object (DLSS mode,
// preset) then keep the saved XeSS model and UI owner.
void CarryControlField(std::string& content, const std::string& saved, const char* name)
{
    size_t offset = 0, existing = 0;
    if (FindJsonValue(content, name, offset) || !FindJsonValue(saved, name, existing)) return;
    size_t end = existing;
    while (end < saved.size() && saved[end] != ',' && saved[end] != '}'
        && saved[end] != '\r' && saved[end] != '\n') ++end;
    const size_t close = content.find_last_not_of(" \t\r\n");
    if (end == existing || close == std::string::npos || content[close] != '}') return;
    content.insert(close, std::string(",\"") + name + "\":" + saved.substr(existing, end-existing));
}

BOOL WriteNativeControl(const std::string& input)
{
    std::string content = input;
    std::string saved;
    if (ReadTextFile(gNativeConfigPath, saved))
    {
        for (const char* name : {"xessModel", "optiScalerUi", "dlssSrPreset", "dlssRrPreset", "xessVulkan", "xessVulkanFrames",
                 "xemfgStandalone", "xemfgFrames", "xemfgDynamic", "xemfgTargetFps", "xemfgRounding", "xemfgAllow1x",
                 "xemfgTargetAsCap"})
            CarryControlField(content, saved, name);
        for (const auto& field : dlss5::settings_json::kFields)
            CarryControlField(content, saved, field.key);
    }
    if (content.empty() || content.size() > 4096) return FALSE;
    std::error_code error;
    std::filesystem::create_directories(
        std::filesystem::path(gNativeConfigPath).parent_path(), error);
    if (error)
        return FALSE;
    const std::wstring temporary = gNativeConfigPath + L".tmp";
    HANDLE file = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr,
        CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return FALSE;
    DWORD written = 0;
    const BOOL write = WriteFile(file, content.data(), static_cast<DWORD>(content.size()),
        &written, nullptr);
    const BOOL flushed = write && written == content.size()
        && FlushFileBuffers(file);
    CloseHandle(file);
    if (!flushed || !MoveFileExW(temporary.c_str(), gNativeConfigPath.c_str(),
            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
    {
        DeleteFileW(temporary.c_str());
        return FALSE;
    }
    return TRUE;
}

BOOL WINAPI NativeFileApplyControl(uint32_t multiplier, BOOL dynamicMode,
    uint32_t dynamicTargetFrameRate, BOOL dynamicExperimental56,
    BOOL generatedOnlyDebug)
{
    const BOOL followGame = multiplier == 0 && dynamicMode == FALSE;
    MfgUnlockReShadeSnapshot current{};
    if (!NativeFileGetSnapshot(&current)) return FALSE;
    if (!xess_ui::DlssMfgFamily(current.gpuFamily)) return FALSE;
    if (UiAmpere())
    {
        if (!ampere_policy::ControlValid(followGame ? 2u : multiplier, dynamicMode != FALSE,
                dynamicExperimental56 != FALSE, generatedOnlyDebug != FALSE)) return FALSE;
    }
    const uint32_t safeMaximumMultiplier = current.safeMaximumMultiplier;
    if ((!followGame && multiplier > safeMaximumMultiplier)
        || (dynamicMode && (!current.dynamicMfgSupportKnown || !current.dynamicMfgSupported
            || !nvidia_mfg_policy::DynamicRangeFits(current.numFramesToGenerateMax, safeMaximumMultiplier)
            || (UiAmpere() && safeMaximumMultiplier < 6)))
        || current.dlssgPresetRequested > 3)
        return FALSE;
    multiplier = followGame
        ? 2u : std::clamp(multiplier, UiAmpere() ? 1u : 2u, safeMaximumMultiplier);
    dynamicTargetFrameRate = std::min(dynamicTargetFrameRate, 1000u);
    BOOL intervalLogging = UiAmpere() ? FALSE : TRUE;
    if (UiAmpere())
    {
        std::string existingControl;
        if (ReadTextFile(gNativeConfigPath, existingControl))
            ParseJsonBool(existingControl, "intervalLogging", intervalLogging);
    }
    return WriteNativeControl(NativeControlJson(multiplier, followGame, dynamicMode,
        dynamicTargetFrameRate, generatedOnlyDebug, intervalLogging,
        current.dlssgPresetRequested, current.vsyncMode, current.reflexFrameLimitFps));
}

bool SetUnsignedControlField(std::string& content, const char* name, uint32_t value)
{
    size_t offset = 0;
    if (FindJsonValue(content, name, offset))
    {
        uint32_t previous = 0;
        if (!ParseJsonInteger(content, name, previous)) return false;
        size_t end = offset;
        while (end < content.size() && content[end] >= '0' && content[end] <= '9') ++end;
        content.replace(offset, end-offset, std::to_string(value));
    }
    else
    {
        const size_t end = content.find_last_not_of(" \t\r\n");
        if (end == std::string::npos || content[end] != '}') return false;
        content.insert(end, std::string(",\"") + name + "\":" + std::to_string(value));
    }
    return true;
}

// key: "dlssgPreset" (0..3, checked here), "dlssSrPreset" or "dlssRrPreset"
// (the caller validates those values).
BOOL NativeFilePersistPreset(const char* key, uint32_t preset)
{
    if (std::strcmp(key, "dlssgPreset") == 0 && preset > 3) return FALSE;
    std::string content;
    if (ReadTextFile(gNativeConfigPath, content))
    {
        MfgUnlockReShadeSnapshot saved{};
        if (!ParseNativeControlSnapshot(content, &saved)) return FALSE;
    }
    else
    {
        // Never overwrite an unreadable existing file as though it were absent.
        const DWORD attributes = GetFileAttributesW(gNativeConfigPath.c_str());
        const DWORD error = attributes == INVALID_FILE_ATTRIBUTES ? GetLastError() : ERROR_SUCCESS;
        if (attributes != INVALID_FILE_ATTRIBUTES
            || (error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND)) return FALSE;
        MfgUnlockReShadeSnapshot current{};
        if (!NativeFileGetSnapshot(&current)) return FALSE;
        content = NativeControlJson(current.desiredMultiplier, current.followGameMode,
            current.dynamicMode, current.dynamicTargetFrameRate, current.generatedOnlyDebug,
            current.intervalLoggingEnabled, current.dlssgPresetRequested,
            current.vsyncMode, current.reflexFrameLimitFps);
    }
    // Preset changes do not need Dynamic capability or an active FG or DLSS
    // feature. The backend's immutable preset state decides whether the saved
    // request belongs to this process or the next restart.
    if (!SetUnsignedControlField(content, key, preset)
        || !SetUnsignedControlField(content, "version", 13)) return FALSE;
    return WriteNativeControl(content);
}

BOOL NativeFilePersistDlss5(const dlss5::Settings& settings)
{
    std::string content;
    if(ReadTextFile(gNativeConfigPath,content)) {
        MfgUnlockReShadeSnapshot saved{};if(!ParseNativeControlSnapshot(content,&saved))return FALSE;
    }else{
        const auto attributes=GetFileAttributesW(gNativeConfigPath.c_str());const auto error=GetLastError();
        if(attributes!=INVALID_FILE_ATTRIBUTES || (error!=ERROR_FILE_NOT_FOUND && error!=ERROR_PATH_NOT_FOUND))return FALSE;
        MfgUnlockReShadeSnapshot current{};if(!NativeFileGetSnapshot(&current))return FALSE;
        content=NativeControlJson(current.desiredMultiplier,current.followGameMode,current.dynamicMode,
            current.dynamicTargetFrameRate,current.generatedOnlyDebug,current.intervalLoggingEnabled,
            current.dlssgPresetRequested,current.vsyncMode,current.reflexFrameLimitFps);
    }
    if(!dlss5::settings_json::Write(content,settings))return FALSE;
    return WriteNativeControl(content);
}

BOOL NativeFilePersistPresentation(uint32_t vsyncMode, uint32_t reflexFrameLimitFps)
{
    if (vsyncMode > 2 || reflexFrameLimitFps > 1000) return FALSE;
    std::string content;
    if (ReadTextFile(gNativeConfigPath, content))
    {
        MfgUnlockReShadeSnapshot saved{};
        if (!ParseNativeControlSnapshot(content, &saved)
            || !SetUnsignedControlField(content, "vsyncMode", vsyncMode)
            || !SetUnsignedControlField(content, "reflexFrameLimitFps", reflexFrameLimitFps)
            || !SetUnsignedControlField(content, "version", 13)) return FALSE;
    }
    else
    {
        const DWORD attributes = GetFileAttributesW(gNativeConfigPath.c_str());
        const DWORD error = attributes == INVALID_FILE_ATTRIBUTES ? GetLastError() : ERROR_SUCCESS;
        if (attributes != INVALID_FILE_ATTRIBUTES
            || (error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND)) return FALSE;
        MfgUnlockReShadeSnapshot current{};
        if (!NativeFileGetSnapshot(&current)) return FALSE;
        content = NativeControlJson(current.desiredMultiplier, current.followGameMode,
            current.dynamicMode, current.dynamicTargetFrameRate, current.generatedOnlyDebug,
            current.intervalLoggingEnabled, current.dlssgPresetRequested,
            vsyncMode, reflexFrameLimitFps);
    }
    return WriteNativeControl(content);
}

// xemfgsa.1: XeMFG standalone's switch and frame count. The frame count applies live; the switch at the game's next
// swap chain (a restart). xemfgsa.6: and its Dynamic MFG settings (`x`'s xemfgDynamic..xemfgTargetAsCap), live.
BOOL NativeFilePersistXemfg(bool on, uint32_t frames, const xess_ui::State& x)
{
    frames = std::clamp<uint32_t>(frames, 2, 6);
    std::string content;
    if (ReadTextFile(gNativeConfigPath, content))
    {
        MfgUnlockReShadeSnapshot saved{};
        if (!ParseNativeControlSnapshot(content, &saved)) return FALSE;
    }
    else
    {
        const DWORD attributes = GetFileAttributesW(gNativeConfigPath.c_str());
        const DWORD error = attributes == INVALID_FILE_ATTRIBUTES ? GetLastError() : ERROR_SUCCESS;
        if (attributes != INVALID_FILE_ATTRIBUTES
            || (error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND)) return FALSE;
        MfgUnlockReShadeSnapshot current{};
        if (!NativeFileGetSnapshot(&current)) return FALSE;
        content = NativeControlJson(current.desiredMultiplier, current.followGameMode,
            current.dynamicMode, current.dynamicTargetFrameRate, current.generatedOnlyDebug,
            current.intervalLoggingEnabled, current.dlssgPresetRequested,
            current.vsyncMode, current.reflexFrameLimitFps);
    }
    if (!SetUnsignedControlField(content, "xemfgStandalone", on ? 1u : 0u)
        || !SetUnsignedControlField(content, "xemfgFrames", frames)
        || !SetUnsignedControlField(content, "xemfgDynamic", x.xemfgDynamic ? 1u : 0u)
        || !SetUnsignedControlField(content, "xemfgTargetFps", std::min<uint32_t>(x.xemfgTargetFps, 1000))
        || !SetUnsignedControlField(content, "xemfgRounding", std::min<uint32_t>(x.xemfgRounding, 2))
        || !SetUnsignedControlField(content, "xemfgAllow1x", x.xemfgAllow1x ? 1u : 0u)
        || !SetUnsignedControlField(content, "xemfgTargetAsCap", x.xemfgTargetAsCap ? 1u : 0u)
        || !SetUnsignedControlField(content, "version", 13)) return FALSE;
    return WriteNativeControl(content);
}

void WINAPI NativeFileSetFrontendAttached(BOOL)
{
}

BOOL WINAPI NativeFileRegisterD3D12Device(ID3D12Device*)
{
    return TRUE;
}

BOOL WINAPI NativeFileRegisterD3D12Queue(ID3D12CommandQueue*)
{
    return TRUE;
}

BOOL WINAPI NativeFileRegisterD3D12Swapchain(
    IDXGISwapChain*, IUnknown*)
{
    return TRUE;
}

BOOL WINAPI NativeFileUnregisterD3D12Swapchain(IDXGISwapChain*)
{
    return TRUE;
}

bool ConnectBackend() noexcept
{
    if (gBackendConnected.load(std::memory_order_acquire))
        return true;

    AcquireSRWLockExclusive(&gBackendConnectionLock);
    if (gBackendConnected.load(std::memory_order_acquire))
    {
        ReleaseSRWLockExclusive(&gBackendConnectionLock);
        return true;
    }
    HMODULE core = gSelf;
    HMODULE shim = gSelf;
    if (!core || !shim || !GetProcAddress(core, "MfgUnlockCoreLoaded")
        || !ResolveNativeFilePaths())
    {
        ReleaseSRWLockExclusive(&gBackendConnectionLock);
        return false;
    }
    gBackend = core;
    gBackendInterface = {};
    gSampleFrameTelemetry = reinterpret_cast<
        MfgUnlockSampleFrameTelemetryFn>(GetProcAddress(
            core, "MfgUnlockSampleFrameTelemetry"));
    gBackendInterface.getSnapshot = &NativeFileGetSnapshot;
    gBackendInterface.applyControl = &NativeFileApplyControl;
    gBackendInterface.registerD3D12Device =
        &NativeFileRegisterD3D12Device;
    gBackendInterface.registerD3D12Queue =
        &NativeFileRegisterD3D12Queue;
    gBackendInterface.registerD3D12Swapchain =
        &NativeFileRegisterD3D12Swapchain;
    gBackendInterface.unregisterD3D12Swapchain =
        &NativeFileUnregisterD3D12Swapchain;
    gBackendInterface.setFrontendAttached =
        &NativeFileSetFrontendAttached;
    gBackendKind.store(BackendKind::eNativeFile,
        std::memory_order_release);
    gBackendConnected.store(true, std::memory_order_release);
    ReleaseSRWLockExclusive(&gBackendConnectionLock);
    return true;
}

void DisconnectBackend() noexcept
{
    gBackendConnected.store(false, std::memory_order_release);
    gBackendInterface = {};
    gBackend = nullptr;
    gSampleFrameTelemetry = nullptr;
    gTelemetrySwapchain.store(0, std::memory_order_release);
    gTelemetrySwapchainTick.store(0, std::memory_order_release);
    gBackendKind.store(BackendKind::eNone, std::memory_order_release);
}


void PersistSettings(reshade::api::effect_runtime* runtime)
{
    reshade::set_config_value(runtime, kConfigSection,
        "FollowGameMode", gFollowGameMode);
    reshade::set_config_value(runtime, kConfigSection,
        "Multiplier", gMultiplier);
    reshade::set_config_value(runtime, kConfigSection,
        "DynamicMode", gDynamicMode);
    reshade::set_config_value(runtime, kConfigSection,
        "DynamicTargetFrameRate", gDynamicTargetFrameRate);
    reshade::set_config_value(runtime, kConfigSection,
        "DynamicLockToRefreshRate", gDynamicLockToRefreshRate);
    reshade::set_config_value(runtime, kConfigSection,
        "DynamicCustomTargetFrameRate", gDynamicCustomTargetFrameRate);
    reshade::set_config_value(runtime, kConfigSection,
        "GeneratedOnlyDebug", gGeneratedOnlyDebug);
}

void ApplySettings(reshade::api::effect_runtime* runtime, bool persist)
{
    MfgUnlockReShadeSnapshot capacity{};
    if (!BackendGetSnapshot(&capacity))
    {
        if (persist)
        {
            gLastApplyAttempted = true;
            gLastApplyAccepted = false;
            gLastNativeConfigPersisted = false;
        }
        return;
    }
    if (UiAmpere())
    {
        gMultiplier = std::clamp(gMultiplier, 1, 6);
    }
    else
    {
        gMultiplier = std::clamp(gMultiplier, 2, 6);
    }
    gDynamicExperimental56 = false;
    gDynamicCustomTargetFrameRate = std::clamp(
        gDynamicCustomTargetFrameRate, 1, 1000);
    gDynamicTargetFrameRate = gDynamicLockToRefreshRate
        ? 0 : gDynamicCustomTargetFrameRate;
    const BOOL accepted = BackendApplyControl(
        gFollowGameMode ? 0u : static_cast<uint32_t>(gMultiplier),
        !gFollowGameMode && gDynamicMode ? TRUE : FALSE,
        static_cast<uint32_t>(gDynamicTargetFrameRate),
        gDynamicExperimental56 ? TRUE : FALSE,
        gGeneratedOnlyDebug ? TRUE : FALSE);
    if (persist)
    {
        const BOOL companionConfigPersisted = TRUE;
        const BOOL nativeConfigPersisted = accepted;
        gLastNativeConfigPersisted = companionConfigPersisted != FALSE
            && nativeConfigPersisted != FALSE;
        gLastApplyAttempted = true;
        gLastApplyAccepted = accepted != FALSE;
        if (accepted) PersistSettings(runtime);
    }
}

void LoadSettings()
{
    gPresetSaveFailed = false;
    gSrPresetSaveFailed = false;
    gRrPresetSaveFailed = false;
    gPresentationSaveFailed = false;
    gDefaultDockInitialized = false;
    reshade::get_config_value(nullptr, kConfigSection,
        "DefaultDockInitialized", gDefaultDockInitialized);

    MfgUnlockReShadeSnapshot snapshot{};
    bool snapshotLoaded = BackendGetSnapshot(&snapshot) != FALSE;
    // Saved intent is independent of a transient status-file read failure.
    if (BackendUsesNativeFile())
    {
        MfgUnlockReShadeSnapshot saved{};
        if (ReadNativeControlSnapshot(&saved)) { snapshot = saved; snapshotLoaded = true; }
    }
    if (snapshotLoaded)
    {
        gFollowGameMode = snapshot.followGameMode != FALSE;
        gMultiplier = static_cast<int>(snapshot.desiredMultiplier);
        gDynamicMode = snapshot.dynamicMode != FALSE;
        gDlssgPreset = static_cast<int>(snapshot.dlssgPresetRequested);
        gDlssSrPreset = dlss_sr::IsValidSelection(snapshot.dlssSr.presetRequested)
            ? snapshot.dlssSr.presetRequested : dlss_sr::kGameOrDriver;
        gDlssRrPreset = dlss_sr::IsValidRrSelection(snapshot.dlssRr.presetRequested)
            ? snapshot.dlssRr.presetRequested : dlss_sr::kGameOrDriver;
        gDlss5=snapshot.dlss5;
        gVsyncMode = static_cast<int>(snapshot.vsyncMode);
        gReflexFrameLimitFps = static_cast<int>(snapshot.reflexFrameLimitFps);
        gDynamicTargetFrameRate = static_cast<int>(
            snapshot.dynamicTargetFrameRate);
        if (gReflexFrameLimitFps > 0) gReflexCustomLimitFps = gReflexFrameLimitFps;
        gDynamicLockToRefreshRate = gDynamicTargetFrameRate == 0;
        if (!gDynamicLockToRefreshRate)
        {
            gDynamicCustomTargetFrameRate = gDynamicTargetFrameRate;
        }
        else
        {
            reshade::get_config_value(nullptr, kConfigSection,
                "DynamicCustomTargetFrameRate",
                gDynamicCustomTargetFrameRate);
        }
        gDynamicCustomTargetFrameRate = std::clamp(
            gDynamicCustomTargetFrameRate, 1, 1000);
        gDynamicExperimental56 = false;
        gGeneratedOnlyDebug = snapshot.generatedOnlyDebug != FALSE;
    }

    // The early-loaded core owns the universal control file. ReShade may
    // initialize much later, so stale ReShade.ini values must not overwrite
    // the request which was already consumed during Streamline startup.
    // No startup/migration write is allowed merely because status is absent.
}

const char* StreamlineResultLabel(int32_t result) noexcept
{
    switch (result)
    {
    case 0:
        return "ok";
    case 34:
        return "feature failed to load";
    case 38:
        return "invalid state/capacity";
    case 39:
        return "accepted; low VRAM warning";
    default:
        return "see Streamline result";
    }
}

const char* YesNo(BOOL value) noexcept
{
    return value ? "yes" : "no";
}

// Menu layout. Stock ImGui colours; only status values are tinted.
enum class Tone { Normal, Good, Pending, Bad, Dim };

ImVec4 ToneColor(Tone tone)
{
    switch (tone)
    {
    case Tone::Good: return ImVec4(0.50f, 0.79f, 0.50f, 1.0f);
    case Tone::Pending: return ImVec4(0.88f, 0.69f, 0.31f, 1.0f);
    case Tone::Bad: return ImVec4(0.94f, 0.45f, 0.42f, 1.0f);
    case Tone::Dim: return ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled);
    default: return ImGui::GetStyleColorVec4(ImGuiCol_Text);
    }
}

void HelpMarker(const char* text)
{
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("%s", text);
}

void Section(const char* title)
{
    ImGui::Spacing();
    ImGui::SeparatorText(title);
}

// Tabs: plain labels over one hairline. The open tab is white and underlined in
// the accent, the others grey (stock ImGui tabs are filled blue boxes).
ImVec2 TabPadding()
{
    const ImGuiStyle& style = ImGui::GetStyle();
    const float font = ImGui::GetFontSize();
    return ImVec2(style.FramePadding.x + font * 0.25f, style.FramePadding.y + font * 0.20f);
}

// The tab bar reads these while it lays out and draws each tab, so they are
// pushed around BeginTabBar and each BeginTabItem, never around a tab's page.
void PushTabStyle()
{
    const ImVec2 padding = TabPadding();
    for (const ImGuiCol col : {ImGuiCol_Tab, ImGuiCol_TabSelected, ImGuiCol_TabDimmed, ImGuiCol_TabDimmedSelected})
        ImGui::PushStyleColor(col, IM_COL32(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_TabHovered, ImVec4(1.0f, 1.0f, 1.0f, 0.06f));
    ImGui::PushStyleVar(ImGuiStyleVar_TabBarBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_TabRounding, ImGui::GetStyle().FrameRounding);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, padding);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemInnerSpacing,
        ImVec2(ImGui::GetFontSize() * 0.25f, ImGui::GetStyle().ItemInnerSpacing.y));
}

void PopTabStyle()
{
    ImGui::PopStyleVar(4);
    ImGui::PopStyleColor(5);
}

// BeginTabBar with the tab look; EndTabBar as usual.
bool BeginTabs(const char* id)
{
    PushTabStyle();
    const bool open = ImGui::BeginTabBar(id, ImGuiTabBarFlags_NoTooltip);
    PopTabStyle();
    if (!open) return false;
    const ImRect bar = ImGui::GetCurrentTabBar()->BarRect;
    ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(bar.Min.x, bar.Max.y - 1.0f), bar.Max,
        ImGui::GetColorU32(ImGuiCol_Separator));
    return true;
}

// BeginTabItem with the tab look; EndTabItem as usual.
bool TabItem(const char* label)
{
    // The label colour is set before ImGui draws it: white for the tab shown this
    // frame (its pending selection before the bar's layout) or hovered last frame.
    const ImGuiTabBar* bar = ImGui::GetCurrentTabBar();
    const ImGuiID id = ImGui::GetID(label);
    const ImGuiID shown = bar->NextSelectedTabId ? bar->NextSelectedTabId : bar->VisibleTabId;
    const ImVec4 text = ImGui::GetStyleColorVec4(ImGuiCol_Text);
    ImGui::PushStyleColor(ImGuiCol_Text, shown == id || ImGui::GetCurrentContext()->HoveredIdPreviousFrame == id
        ? text : ImLerp(ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled), text, 0.25f));
    PushTabStyle();
    const bool open = ImGui::BeginTabItem(label);
    PopTabStyle();
    ImGui::PopStyleColor();
    if (open)
    {
        const ImVec2 min = ImGui::GetItemRectMin();
        const ImVec2 max = ImGui::GetItemRectMax();
        const float thickness = IM_ROUND(ImGui::GetFontSize() * 0.12f);
        const float inset = IM_ROUND(TabPadding().x * 0.5f);
        ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(min.x + inset, max.y - thickness),
            ImVec2(max.x - inset, max.y), ImGui::GetColorU32(ImGuiCol_CheckMark), thickness * 0.5f);
    }
    return open;
}

// Controls stay usable-looking while status is stale, so an open popup keeps
// its appearance across a missed read. Item-level disables inside still dim.
void BeginStaleDisabled(bool stale)
{
    ImGui::PushStyleVar(ImGuiStyleVar_DisabledAlpha, 1.0f);
    ImGui::BeginDisabled(stale);
    ImGui::PopStyleVar();
}

// Label | value grid shared by controls and status rows. The label column fits
// the widest label drawn last frame (translations differ in length): at least
// 8 em, at most 14 em, the same in every grid so the columns line up.
float gLabelWidth = 0.0f;     // widest label of the previous frame, in pixels
float gLabelWidthSeen = 0.0f; // widest label so far this frame

bool BeginGrid(const char* id)
{
    if (!ImGui::BeginTable(id, 2, ImGuiTableFlags_NoSavedSettings)) return false;
    const float font = ImGui::GetFontSize();
    ImGui::TableSetupColumn("##label", ImGuiTableColumnFlags_WidthFixed,
        std::clamp(gLabelWidth, font * 8.0f, font * 14.0f));
    ImGui::TableSetupColumn("##value", ImGuiTableColumnFlags_WidthStretch);
    return true;
}

void RowLabel(const char* label, bool control)
{
    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(0);
    gLabelWidthSeen = std::max(gLabelWidthSeen, ImGui::CalcTextSize(label).x);
    if (control) ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("%s", label);
    ImGui::TableSetColumnIndex(1);
}

// Controls share one width, leaving room for a (?) marker.
void ControlRow(const char* label)
{
    RowLabel(label, true);
    const float font = ImGui::GetFontSize();
    ImGui::SetNextItemWidth(std::max(font * 4.0f,
        std::min(font * 13.0f, ImGui::GetContentRegionAvail().x - font * 1.8f)));
}

void StatusRow(const char* label, Tone tone, const char* format, ...)
{
    RowLabel(label, false);
    va_list args;
    va_start(args, format);
    ImGui::PushStyleColor(ImGuiCol_Text, ToneColor(tone));
    ImGui::TextWrappedV(format, args);
    ImGui::PopStyleColor();
    va_end(args);
}

// A short coloured value and a dim detail, e.g. "Active  tracing converted hair".
void StatusDetail(const char* label, Tone tone, const char* value, const char* detail)
{
    RowLabel(label, false);
    ImGui::TextColored(ToneColor(tone), "%s", value);
    if (detail && detail[0])
    {
        ImGui::SameLine();
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextDisabled("%s", detail);
        ImGui::PopTextWrapPos();
    }
}

// Visible notes are for states the user acts on or waits for; explanations
// go into (?) markers.
void Note(Tone tone, const char* format, ...)
{
    va_list args;
    va_start(args, format);
    ImGui::PushStyleColor(ImGuiCol_Text, ToneColor(tone));
    ImGui::TextWrappedV(format, args);
    ImGui::PopStyleColor();
    va_end(args);
}

std::string Grouped(uint64_t value)
{
    std::string digits = std::to_string(value);
    for (ptrdiff_t at = static_cast<ptrdiff_t>(digits.size()) - 3; at > 0; at -= 3)
        digits.insert(static_cast<size_t>(at), 1, ',');
    return digits;
}

// The overlay draws this beside the menu key, after the panel. English; translated when drawn.
const char* gFooterStatus = "";
Tone gFooterTone = Tone::Dim;

void SetFooter(const char* text, Tone tone)
{
    gFooterStatus = text;
    gFooterTone = tone;
}

// Pinned FPS readout: the overlay draws it in the top-left corner while the
// menu is closed. Toggled beside the FPS row; [Overlay] PinnedFps.
std::atomic<bool> gPinnedFps{false};

// xemfgsa.5: `tooltip` names the frame rates the readout shows (DLSS's by default, XeMFG's beside XeMFG's row).
void PinnedFpsToggle(const char* tooltip = nullptr)
{
    ImGui::SameLine();
    // No vertical frame padding, so the checkbox keeps the status row's height.
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(ImGui::GetStyle().FramePadding.x, 0.0f));
    bool pinned = gPinnedFps.load(std::memory_order_acquire);
    if (ImGui::Checkbox(l10n::Label("Pin"), &pinned))
    {
        gPinnedFps.store(pinned, std::memory_order_release);
        const auto path = standalone_ui::SettingsPath("RTX40MFG-UI.ini");
        if (!path.empty()) WritePrivateProfileStringW(L"Overlay", L"PinnedFps", pinned ? L"1" : L"0", path.c_str());
    }
    ImGui::PopStyleVar();
    ImGui::SetItemTooltip("%s", tooltip ? tooltip : T("Shows the real and DLSS frame rates in the top-left corner\n"
        "while the menu is closed. Saved for the next launch."));
}

const char* DlssgPresetName(uint32_t preset) noexcept
{
    switch (preset)
    {
    case 0: return "Game / driver";
    case 1: return "A";
    case 2: return "B";
    case 3: return "C";
    default: return "Unknown";
    }
}

// Preset readback for the Status grid.
void PresetStatusRow(const MfgUnlockReShadeSnapshot& snapshot)
{
    if (UiAmpere() && snapshot.ampereLegacySinglePreset)
    {
        if (snapshot.dlssgPresetObservedValid && snapshot.dlssgPresetObserved == 1)
            StatusDetail(T("DLSS-G preset"), Tone::Good, T("Original"), T("observed"));
        else
            StatusDetail(T("DLSS-G preset"), Tone::Pending, T("Original"), T("waiting for the provider"));
        return;
    }
    const uint32_t activePreset = snapshot.dlssgPresetSelectionFrozen
        ? snapshot.dlssgPresetLatched : snapshot.dlssgPresetRequested;
    char value[32]{};
    snprintf(value, sizeof(value), T("Preset %s"), DlssgPresetName(activePreset));
    if (activePreset == 0)
        StatusDetail(T("DLSS-G preset"), Tone::Normal, T("Game / driver"), T("managed by the game or driver"));
    else if (activePreset == 3 && snapshot.dlssgPresetReadCount > 0 && !snapshot.dlssgPresetCAvailable)
        StatusDetail(T("DLSS-G preset"), Tone::Pending, value, T("needs DLSS-G 310.9.16; game / driver in use"));
    else if (snapshot.dlssgPresetObservedValid && snapshot.dlssgPresetObserved == activePreset
        && snapshot.dlssgPresetOverrideReadCount > 0)
        StatusDetail(T("DLSS-G preset"), Tone::Good, value, T("override supplied"));
    else if (snapshot.dlssgPresetOverrideFailure != 0)
        StatusDetail(T("DLSS-G preset"), Tone::Bad, value, T("override unavailable"));
    else
        StatusDetail(T("DLSS-G preset"), Tone::Pending, value, T("waiting for the provider"));
}

// Preset messages the user acts on.
void PresetNotes(const MfgUnlockReShadeSnapshot& snapshot)
{
    if (UiAmpere() && snapshot.ampereLegacySinglePreset)
    {
        Note(Tone::Dim, "%s", snapshot.dlssgPresetRequested == 2
            ? T("This DLSS-G provider supports its original preset only. Preset B stays saved for providers that support it.")
            : T("This DLSS-G provider supports its original preset only."));
    }
    else if (snapshot.dlssgPresetSelectionFrozen && snapshot.dlssgPresetLiveSwitch == 0
        && snapshot.dlssgPresetRequested != snapshot.dlssgPresetLatched)
    {
        // The backend re-creates frame generation once with the saved preset; with the
        // game's frame generation off, its next start reads the preset.
        if (snapshot.dlssgPresetSwitching || snapshot.gameFrameGenerationOn)
            Note(Tone::Pending, "%s", T("Restarting frame generation with the new preset..."));
    }
    else if (snapshot.dlssgPresetSwitching)
        Note(Tone::Pending, "%s", T("Restarting frame generation with the new preset..."));
    else if (snapshot.dlssgPresetSelectionFrozen
        && snapshot.dlssgPresetRequested != snapshot.dlssgPresetLatched)
    {
        if (snapshot.dlssgPresetRequested == 0)
            Note(Tone::Pending, T("The game / driver preset is saved. Restart the game to use it."));
        else
            Note(Tone::Pending, T("Preset %s is saved. Restart the game to use it."),
                DlssgPresetName(snapshot.dlssgPresetRequested));
    }
    const uint32_t activePreset = snapshot.dlssgPresetSelectionFrozen
        ? snapshot.dlssgPresetLatched : snapshot.dlssgPresetRequested;
    if (!(UiAmpere() && snapshot.ampereLegacySinglePreset) && activePreset == 3
        && snapshot.dlssgPresetReadCount > 0 && !snapshot.dlssgPresetCAvailable)
        Note(Tone::Dim, T("Preset C needs DLSS-G 310.9.16 or newer (NVIDIA app). This game's"
            " frame generation keeps the game / driver preset."));
    if (gPresetSaveFailed)
        Note(Tone::Bad, T("The preset selection could not be saved."));
}

// Combo text of a preset option: a letter reads as "Preset <letter>", other
// options are msgids. UI thread; the text is valid until the next call.
const char* PresetOption(const char* option) noexcept
{
    static char label[32]{};
    if (option[0] == '\0' || option[1] != '\0') return T(option);
    snprintf(label, sizeof(label), T("Preset %s"), option);
    return label;
}

// Preset control row. Updates snapshot.dlssgPresetRequested to the selection
// for the status row.
void PresetControlRow(MfgUnlockReShadeSnapshot& snapshot, bool statusStale)
{
    // Preset C needs DLSS-G 310.9.16 or newer (RTX 40 and, since 931, the RTX 30 port).
    const int maximumPreset = 3;
    if (!statusStale && snapshot.dlssgPresetRequested <= 3)
        gDlssgPreset = static_cast<int>(snapshot.dlssgPresetRequested);
    const int previous = gDlssgPreset;
    bool changed = false;
    static const char* options[] = {N_("Game / driver default"), "A", "B", N_("Preset C (performance)")};
    const bool legacyPreset = UiAmpere() && snapshot.ampereLegacySinglePreset;
    ControlRow(T("DLSS-G preset"));
    ImGui::BeginDisabled(legacyPreset);
    if (ImGui::BeginCombo("##dlssgpreset", legacyPreset ? T("Original preset") : PresetOption(options[std::clamp(gDlssgPreset, 0, 3)])))
    {
        for (int preset = 0; preset <= maximumPreset; ++preset)
        {
            if (ImGui::Selectable(PresetOption(options[preset]), gDlssgPreset == preset))
            {
                gDlssgPreset = preset;
                changed = preset != previous;
            }
        }
        ImGui::EndCombo();
    }
    ImGui::EndDisabled();
    HelpMarker(T("The DLSS frame generation model preset.\n"
        "Preset C is NVIDIA's lighter Frame Generation mode. It needs DLSS-G 310.9.16 or newer,\n"
        "from the NVIDIA app's DLSS override; other providers keep the game / driver preset.\n"
        "A change made while frame generation runs restarts it once, with a short pause;\n"
        "some older DLSS-G versions apply it after a game restart."));
    if (changed)
    {
        const BOOL saved = NativeFilePersistPreset("dlssgPreset", static_cast<uint32_t>(gDlssgPreset));
        gLastApplyAttempted = true;
        gLastApplyAccepted = saved != FALSE;
        gLastNativeConfigPersisted = saved != FALSE;
        gPresetSaveFailed = !saved;
        if (!saved) gDlssgPreset = previous;
    }
    snapshot.dlssgPresetRequested = static_cast<uint32_t>(gDlssgPreset);
}

// DLSS Super Resolution and Ray Reconstruction (V1.6.0). The game runs
// one or the other, so one section shows the controls of the running feature.
const char* SrPresetLetter(uint32_t preset) noexcept
{
    static constexpr const char* kLetters[]{"", "A", "B", "C", "D", "E", "F", "G",
        "H", "I", "J", "K", "L", "M", "N", "O"};
    return preset < std::size(kLetters) ? kLetters[preset] : "";
}

// A render at the output size is DLAA whatever mode the game created it with: MSFS 2024
// creates its DLAA as Quality (GitHub issue 112).
const char* SrQualityName(const MfgUnlockDlssFeature& view) noexcept
{
    if (view.renderWidth && view.renderWidth == view.outputWidth && view.renderHeight == view.outputHeight)
        return "DLAA";
    switch (view.quality)
    {
    case 0: return T("Performance");
    case 1: return T("Balanced");
    case 2: return T("Quality");
    case 3: return T("Ultra Performance");
    case 4: return T("Ultra Quality");
    case 5: return "DLAA";
    default: return "";
    }
}

// Ray Reconstruction while it runs; otherwise the feature the game still has.
bool RayReconstructionShown(const MfgUnlockReShadeSnapshot& snapshot) noexcept
{
    if (snapshot.dlssRr.running) return true;
    if (snapshot.dlssSr.running) return false;
    if (snapshot.dlssRr.liveFeatures || snapshot.dlssSr.liveFeatures)
        return snapshot.dlssRr.liveFeatures && !snapshot.dlssSr.liveFeatures;
    return snapshot.dlssRr.detected && !snapshot.dlssSr.detected;
}

// Whether the feature runs, and the preset of the running feature and its source.
// Status grid rows, labelled like the preset control.
void DlssStatusRows(const MfgUnlockDlssFeature& view, bool rr)
{
    const char* label = rr ? T("DLSS RR") : T("DLSS SR");
    const char* presetLabel = rr ? T("RR preset") : T("SR preset");
    char value[64]{}, detail[160]{};
    if (view.running)
    {
        snprintf(detail, sizeof(detail), "%s, %ux%u -> %ux%u", SrQualityName(view),
            view.renderWidth, view.renderHeight, view.outputWidth, view.outputHeight);
        StatusDetail(label, Tone::Good, T("Running"), detail);
    }
    else if (view.detected)
        StatusDetail(label, Tone::Dim, T("Idle"), T("not evaluated in the last second"));
    else
    {
        StatusDetail(label, Tone::Dim, T("Not detected"), T("waiting for the game to create DLSS"));
        return;
    }
    if (view.fixedByDriver || (view.activePreset == 0 && view.driverOverrideCached))
    {
        // The provider cached the NVIDIA app's override at its initialization.
        if (view.driverPreset == dlss_sr::kDrsLatest)
            snprintf(detail, sizeof(detail), "%s", T("NVIDIA app override: latest"));
        else
            snprintf(detail, sizeof(detail), T("NVIDIA app override: %s"), SrPresetLetter(view.driverPreset));
        StatusDetail(presetLabel, Tone::Normal, T("Game / driver"), detail);
    }
    else if (view.activePreset != 0)
    {
        // Seen at the provider's DRS read: nothing outranks the creation hints.
        snprintf(value, sizeof(value), T("Preset %s"), SrPresetLetter(view.activePreset));
        StatusDetail(presetLabel, view.drsObserved ? Tone::Good : Tone::Normal, value,
            view.drsObserved ? T("override supplied") : T("sent as the game's preset hint"));
    }
    else if (view.gameHint != 0)
    {
        snprintf(detail, sizeof(detail), T("game hint: %s"), SrPresetLetter(view.gameHint));
        StatusDetail(presetLabel, Tone::Normal, T("Game / driver"), detail);
    }
    else
        StatusDetail(presetLabel, Tone::Normal, T("Game / driver"), T("DLSS default for this mode"));
}

void DlssPresetControlRow(const MfgUnlockDlssFeature& view, bool rr, bool statusStale)
{
    static constexpr uint32_t kSrValues[]{dlss_sr::kGameOrDriver, dlss_sr::kPresetJ, dlss_sr::kPresetK,
        dlss_sr::kPresetL, dlss_sr::kPresetM};
    static const char* srOptions[]{N_("Game / driver default"), "J", "K", "L", "M"};
    static constexpr uint32_t kRrValues[]{dlss_sr::kGameOrDriver, dlss_sr::kRrPresetD, dlss_sr::kRrPresetE,
        dlss_sr::kRrPresetF};
    static const char* rrOptions[]{N_("Game / driver default"), "D", "E", "F"};
    uint32_t& selected = rr ? gDlssRrPreset : gDlssSrPreset;
    const uint32_t* values = rr ? kRrValues : kSrValues;
    const char** options = rr ? rrOptions : srOptions;
    const size_t count = rr ? std::size(kRrValues) : std::size(kSrValues);
    const bool valid = rr ? dlss_sr::IsValidRrSelection(view.presetRequested) : dlss_sr::IsValidSelection(view.presetRequested);
    if (!statusStale && valid) selected = view.presetRequested;
    size_t current = 0;
    for (size_t i = 0; i != count; ++i)
        if (values[i] == selected) current = i;
    const uint32_t previous = selected;
    ControlRow(rr ? T("RR preset") : T("SR preset"));
    if (ImGui::BeginCombo(rr ? "##dlssrrpreset" : "##dlsssrpreset", PresetOption(options[current])))
    {
        for (size_t i = 0; i != count; ++i)
            if (ImGui::Selectable(PresetOption(options[i]), i == current)) selected = values[i];
        ImGui::EndCombo();
    }
    if (rr)
        HelpMarker(T("The DLSS Ray Reconstruction model preset.\n"
            "D is NVIDIA's transformer model; E is the newer transformer model, which games with a\n"
            "depth-of-field guide need. F is NVIDIA's second-generation model (RR2) and needs driver 580\n"
            "or newer; DLSS versions without a preset use their default.\n"
            "A change applies at once while DLSS runs. A preset selected when the game starts also\n"
            "replaces an NVIDIA app preset override for that session."));
    else
        HelpMarker(T("The DLSS Super Resolution model preset.\n"
            "K is NVIDIA's transformer model with the best image quality; J is similar with a little\n"
            "less ghosting and more flicker. L and M are the newer transformer presets NVIDIA uses for\n"
            "Ultra Performance and Performance; DLSS versions without them use their default.\n"
            "A change applies at once while DLSS runs. A preset selected when the game starts also\n"
            "replaces an NVIDIA app preset override for that session."));
    if (selected != previous)
    {
        const BOOL saved = NativeFilePersistPreset(rr ? "dlssRrPreset" : "dlssSrPreset", selected);
        (rr ? gRrPresetSaveFailed : gSrPresetSaveFailed) = !saved;
        if (!saved) selected = previous;
    }
}

void DlssNotes(const MfgUnlockDlssFeature& view, bool rr)
{
    const uint32_t selected = rr ? gDlssRrPreset : gDlssSrPreset;
    if (rr ? gRrPresetSaveFailed : gSrPresetSaveFailed)
        Note(Tone::Bad, "%s", rr ? T("The DLSS RR preset could not be saved.") : T("The DLSS SR preset could not be saved."));
    if (!view.detected) return;
    if (view.fixedByDriver)
    {
        if (selected != dlss_sr::kGameOrDriver)
            Note(Tone::Pending, "%s", T("The NVIDIA app's DLSS preset override is active in this game session. "
                "Restart the game with a preset selected here to switch presets live."));
        return;
    }
    if (view.drsSuppressed && view.driverPresetValid && view.driverPreset != 0
        && selected == dlss_sr::kGameOrDriver)
        Note(Tone::Dim, "%s", T("The NVIDIA app's DLSS preset override is replaced in this session. "
            "It applies again after a restart with Game / driver selected."));
    if (!view.drsObserved && selected != dlss_sr::kGameOrDriver)
        Note(Tone::Dim, "%s", T("The NVIDIA app's DLSS preset override could not be checked in this session. "
            "If one is set, it takes priority."));
    if (!view.running || selected == view.activePreset) return;
    switch (view.liveSwitch)
    {
    case 1:
        Note(Tone::Pending, "%s", T("This game's DLSS cannot change its preset while running. The preset applies "
            "the next time the game starts DLSS (change the DLSS quality mode or restart the game)."));
        break;
    case 2:
        Note(Tone::Bad, "%s", T("DLSS refused the new preset; the previous one stays in use."));
        break;
    case 3:
        Note(Tone::Pending, "%s", T("Waiting to release the previous DLSS preset..."));
        break;
    default:
        Note(Tone::Pending, "%s", rr ? T("Switching DLSS Ray Reconstruction to the new preset...")
            : T("Switching DLSS Super Resolution to the new preset..."));
        break;
    }
}

// A combo over labels[0..count); disabledMask greys out choices.
bool ChoiceCombo(const char* id,uint32_t& value,const char* const* labels,uint32_t count,uint32_t disabledMask=0)
{
    bool changed=false;
    if(ImGui::BeginCombo(id,labels[value<count?value:0])){
        for(uint32_t i=0;i<count;++i){
            ImGui::BeginDisabled((disabledMask>>i)&1u);
            if(ImGui::Selectable(labels[i],value==i) && value!=i){value=i;changed=true;}
            ImGui::EndDisabled();
        }
        ImGui::EndCombo();
    }
    return changed;
}

// A slider edit saves while dragging at most every 150 ms, and on release.
bool Dlss5SliderSave(bool edited)
{
    if(edited)gDlss5Dirty=true;
    return gDlss5Dirty && (ImGui::IsItemDeactivatedAfterEdit() || GetTickCount64()-gDlss5LastSave>=150);
}

// Faces & skin (autoscale.2): its slider and a Same as Structure checkbox share
// the control column, as Resolution Scale and Auto do. While it is ticked the
// strength is -1, which the provider reads as Structure's (its own default),
// and the slider shows that value. Unticking starts from it, so nothing jumps.
bool FacesControl(const char* id,float& character,float structure,bool mask)
{
    const ImGuiStyle& style=ImGui::GetStyle();
    const float box=ImGui::GetFrameHeight()+style.ItemInnerSpacing.x+ImGui::CalcTextSize(T("Same as Structure")).x+style.ItemSpacing.x;
    const bool follows=character<0.0f;
    bool save=false;
    ImGui::PushID(id);
    ImGui::BeginDisabled(!mask);
    ImGui::BeginDisabled(follows);
    float shown=structure;
    ImGui::SetNextItemWidth(std::max(ImGui::GetFontSize()*4.0f,ImGui::CalcItemWidth()-box));
    save|=Dlss5SliderSave(ImGui::SliderFloat("##strength",follows?&shown:&character,0.0f,dlss5::kMaxStrength,"%.2f"));
    ImGui::EndDisabled();
    ImGui::SameLine();
    bool same=follows;
    if(ImGui::Checkbox(l10n::Label("Same as Structure"),&same)){character=same?dlss5::kMinCharacter:structure;save=true;}
    ImGui::EndDisabled();
    ImGui::PopID();
    return save;
}

// Hold to show original: the key's name, Change key (the next key pressed
// binds it, Esc cancels) and Clear. Keys are read below other mods' input
// hooks, so binding also works while OptiScaler's menu is open.
bool gCompareBinding=false;
bool gCompareHeld[256]{};
bool CompareKeyControl(dlss5::Settings& s)
{
    bool changed=false;
    if(gCompareBinding){
        if(PhysicalKeyDown(VK_ESCAPE))gCompareBinding=false;
        for(int key=0x05;key<0xFF && gCompareBinding;++key){
            const bool down=PhysicalKeyDown(key);
            if(down && !gCompareHeld[key] && dlss5::CompareKeyAllowed(static_cast<uint32_t>(key))){
                s.compareKey=static_cast<uint32_t>(key);gCompareBinding=false;changed=true;
            }
            gCompareHeld[key]=down;
        }
    }
    if(gCompareBinding){
        ImGui::TextUnformatted(T("Press a key... Esc cancels"));
        ImGui::SameLine();
        if(ImGui::Button(l10n::Label("Cancel")))gCompareBinding=false;
        return changed;
    }
    if(s.compareKey)ImGui::TextUnformatted(KeyName(static_cast<int>(s.compareKey)).c_str());
    else ImGui::TextDisabled("%s",T("Not set"));
    ImGui::SameLine();
    if(ImGui::Button(l10n::Label("Change key"))){
        // Keys already down (the one that activated the button) do not bind.
        gCompareBinding=true;
        for(int key=0;key<256;++key)gCompareHeld[key]=PhysicalKeyDown(key);
    }
    if(s.compareKey){
        ImGui::SameLine();
        if(ImGui::Button(l10n::Label("Clear"))){s.compareKey=0;changed=true;}
    }
    return changed;
}

const char* Dlss5StateName(uint32_t state,Tone& tone)
{
    switch(static_cast<dlss5::State>(state)){
    case dlss5::State::eOff:tone=Tone::Dim;return T("Off");
    case dlss5::State::eRunning:tone=Tone::Good;return T("Running");
    case dlss5::State::eNotInstalled:tone=Tone::Dim;return T("Provider not found");
    case dlss5::State::eUnsupported:tone=Tone::Dim;return T("GPU or provider unsupported");
    case dlss5::State::eDriverUpdate:tone=Tone::Bad;return T("Driver update required");
    case dlss5::State::eCreateFailed:case dlss5::State::eEvaluateFailed:tone=Tone::Bad;return T("Provider failed");
    case dlss5::State::eOtherMod:tone=Tone::Dim;return T("Provider already in use");
    case dlss5::State::eMissingInputs:tone=Tone::Dim;return T("DLSS inputs unavailable");
    case dlss5::State::eStageUnavailable:tone=Tone::Dim;return T("Hook method unavailable");
    default:tone=Tone::Pending;return T("Waiting for DLSS");
    }
}

// dlss5gate.1: the DLSS 5 tab shows only while its provider (nvngx_dlssnr.dll) sits beside the game or MFG Unlocker,
// or while DLSS 5 runs from one NGX found elsewhere. Saved settings stay while it is hidden.
bool Dlss5TabShown(const MfgUnlockReShadeSnapshot& snapshot) noexcept
{
    return snapshot.dlss5Detected || snapshot.dlss5Route;
}

// The DLSS 5 tab: the RenoDX DLSS add-on's Neural Rendering settings.
void DrawDlss5(const MfgUnlockReShadeSnapshot& snapshot,bool stale)
{
    if(!stale && !gDlss5Dirty && !ImGui::IsAnyItemActive())gDlss5=snapshot.dlss5;
    dlss5::Settings& s=gDlss5;
    const dlss5::Settings before=s;
    bool save=false;
    const bool present=s.enabled && s.stage==static_cast<uint32_t>(dlss5::Stage::ePresent);
    BeginStaleDisabled(stale);

    // The simple view: what most players change. Everything else is under
    // Advanced, which keeps every saved value while it is closed.
    Section(T("Neural Rendering"));
    if(BeginGrid("##dlss5nr")){
        // Menu order: Off, Auto (stage 0), then stages 1-3. The Auto entry names
        // the stage Auto runs: the live one once DLSS 5 runs it, else the policy's.
        const char* const fixedStages[]{T("Before upscaling (faster)"),T("After upscaling"),T("Present")};
        uint32_t chosen=static_cast<uint32_t>(dlss5::auto_mode::AutoStage());
        if(s.enabled && s.stage==static_cast<uint32_t>(dlss5::Stage::eAuto) && snapshot.dlss5Route && !snapshot.dlss5Pending
            && snapshot.dlss5Stage>=1 && snapshot.dlss5Stage<=3)chosen=snapshot.dlss5Stage;
        char automatic[128]{};
        snprintf(automatic,sizeof(automatic),T("Auto: %s"),fixedStages[chosen-1]);
        const char* const stages[]{T("Off"),automatic,fixedStages[0],fixedStages[1],fixedStages[2]};
        uint32_t choice=!s.enabled?0:s.stage==static_cast<uint32_t>(dlss5::Stage::eAuto)?1:s.stage+1;
        // vkstages.1: Vulkan runs Auto and After upscaling; Before upscaling and Present stay listed, disabled.
        const uint32_t unavailable=snapshot.graphicsApi==2u?(1u<<2)|(1u<<4):0u;
        ControlRow(T("Hook Method"));
        if(ChoiceCombo("##dlss5stage",choice,stages,5,unavailable)){s.enabled=choice!=0;if(choice)s.stage=choice==1?0:choice-1;save=true;}
        HelpMarker(T("Auto processes the DLSS output after reconstruction; Resolution Scale Auto keeps its cost in check.\n"
            "Before upscaling processes the DLSS input at render resolution, before DLSS SR, DLAA or Ray Reconstruction.\n"
            "After upscaling processes the DLSS output after reconstruction.\n"
            "Present processes the final image, game UI included, before presentation and frame generation."));
        // Looks set Intensity, Model, Structure, Tone and Faces & skin together;
        // Custom (any other combination) is shown, not chosen.
        const char* const looks[]{T("Subtle"),T("Balanced"),T("Strong"),T("Custom")};
        uint32_t look=static_cast<uint32_t>(dlss5::MatchingLook(s));
        ControlRow(T("Look"));
        if(ChoiceCombo("##dlss5look",look,looks,4,1u<<3) && look<dlss5::kLookCount){dlss5::ApplyLook(s,look);save=true;}
        HelpMarker(T("Sets Intensity, Model, Structure, Tone and Faces & skin together.\n"
            "Balanced is the provider's own default, Subtle halves its change and Strong goes further.\n"
            "Pass Count under Advanced strengthens any look further, at extra GPU cost."));
        ControlRow(T("Intensity"));
        save|=Dlss5SliderSave(ImGui::SliderFloat("##dlss5intensity",&s.intensity,0.0f,1.0f,"%.2f"));
        HelpMarker(T("Strength of the whole neural rendering effect."));
        const char* const models[]{T("Model A"),T("Model B"),T("Model C")};
        ControlRow(T("Model"));
        save|=ChoiceCombo("##dlss5model",s.model,models,3);
        ControlRow(T("Faces & skin"));
        save|=FacesControl("dlss5character",s.character,s.structure,s.characterMask);
        HelpMarker(!s.characterMask?T("Off while Character Mask is off. Turn it on under Advanced.")
            :T("Structure enhancement on detected skin and faces.\nSame as Structure takes the Structure strength, the provider's own default."));
        ControlRow(T("Resolution Scale"));
        {
            // Slider and its Auto checkbox share the control column.
            const ImGuiStyle& style=ImGui::GetStyle();
            const float box=ImGui::GetFrameHeight()+style.ItemInnerSpacing.x+ImGui::CalcTextSize(T("Auto")).x+style.ItemSpacing.x;
            ImGui::SetNextItemWidth(std::max(ImGui::GetFontSize()*4.0f,ImGui::CalcItemWidth()-box));
            save|=Dlss5SliderSave(ImGui::SliderFloat("##dlss5scale",&s.scale,0.0f,dlss5::kMaxScale,s.scaleAuto?T("up to %.0f%%"):"%.0f%%"));
            ImGui::SameLine();ImGui::PushID("dlss5scaleauto");
            save|=ImGui::Checkbox(l10n::Label("Auto"),&s.scaleAuto);
            ImGui::PopID();
        }
        HelpMarker(T("Share of the hook method's resolution that the network processes.\n"
            "100 is native, lower is faster, up to 200 supersamples. Values below 25 use 25.\n"
            "Auto lowers it when DLSS 5 takes more than about a third of each rendered frame, not below 75, and raises it again with headroom, never above the slider."));
        // Boost runs on D3D12 Before and After upscaling; elsewhere it stays in place, disabled.
        // Offered while dlss5::kHalfRateOffered (halfgrid.1).
        if constexpr(dlss5::kHalfRateOffered){
            const bool boostUnavailable=snapshot.graphicsApi==2u || (s.enabled && s.stage==static_cast<uint32_t>(dlss5::Stage::ePresent));
            ControlRow(T("Half rate"));
            ImGui::BeginDisabled(boostUnavailable);
            save|=ImGui::Checkbox("##dlss5boost",&s.boost);
            ImGui::EndDisabled();
            HelpMarker(boostUnavailable?T("Half rate is not available on Vulkan or with the Present hook method yet.")
                :T("Runs the network on every other frame. The frames between apply its last look to their own image, so moving and animated surfaces keep it without trails.\n"
                    "About half the GPU cost. Fine detail follows the game's motion vectors where a surface only moved."));
        }
        ImGui::EndTable();
    }
    // Notes for the controls above; the save result is the previous frame's.
    if(gDlss5SaveFailed)Note(Tone::Bad,"%s",T("DLSS 5 settings could not be saved."));
    // A saved Before upscaling or Present on a Vulkan game stays selected and leaves the frame as is.
    const bool vulkanStage=snapshot.graphicsApi==2u && s.enabled
        && (s.stage==static_cast<uint32_t>(dlss5::Stage::eRender) || s.stage==static_cast<uint32_t>(dlss5::Stage::ePresent));
    if(vulkanStage)
        Note(Tone::Pending,"%s",T("Before upscaling and Present do not run on Vulkan yet. Choose Auto or After upscaling."));
    else if(snapshot.dlss5State==static_cast<uint32_t>(dlss5::State::eStageUnavailable))
        Note(Tone::Dim,"%s",T("This game's swapchain cannot use the Present hook method; the log names the reason. Before or after upscaling still work."));
    if(s.enabled && s.debugView!=static_cast<uint32_t>(dlss5::DebugView::eOff))
        Note(Tone::Pending,"%s",T("Debug view is on (Advanced, Inspect)."));

    // Compare is not part of the look: Reset to defaults keeps it.
    Section(T("Compare"));
    if(BeginGrid("##dlss5compare")){
        ControlRow(T("Split view"));
        save|=ImGui::Checkbox("##dlss5compare",&s.compare);
        HelpMarker(T("Shows the original image left of the line and DLSS 5 to its right.\n"
            "Before upscaling draws no line, because the upscaler would treat it as part of the image."));
        ControlRow(T("Split position"));
        ImGui::BeginDisabled(!s.compare);
        save|=Dlss5SliderSave(ImGui::SliderFloat("##dlss5split",&s.compareSplit,dlss5::kMinCompareSplit,dlss5::kMaxCompareSplit,"%.0f%%"));
        ImGui::EndDisabled();
        ControlRow(T("Hold to show original"));
        save|=CompareKeyControl(s);
        HelpMarker(T("While this key is held, the game shows its original image, so DLSS 5 can be compared in motion.\n"
            "DLSS 5 keeps running, so its GPU cost is unchanged, and the key still reaches the game."));
        ImGui::EndTable();
    }

    Section(T("Status"));
    if(BeginGrid("##dlss5status")){
        Tone tone=Tone::Pending;
        const char* state=Dlss5StateName(snapshot.dlss5State,tone);
        StatusRow(T("State"),stale?Tone::Dim:tone,"%s",stale?T("Waiting for DLSS"):state);
        if(!stale && snapshot.dlss5Route && snapshot.dlss5GpuMs>=0.0f){
            char value[32]{},detail[96]{};
            snprintf(value,sizeof(value),T("%.1f ms"),snapshot.dlss5GpuMs);
            if(snapshot.dlss5FrameMs>0.0f)
                snprintf(detail,sizeof(detail),T("%.0f%% of each rendered frame"),100.0f*snapshot.dlss5GpuMs/snapshot.dlss5FrameMs);
            StatusDetail(T("GPU cost"),Tone::Normal,value,detail);
            // Boost: the GPU cost above is the mean per frame; the network's own cost
            // comes every other frame.
            if(dlss5::kHalfRateOffered && snapshot.dlss5BoostNetworkMs>=0.0f){
                snprintf(value,sizeof(value),T("%.1f ms"),snapshot.dlss5BoostNetworkMs);
                StatusDetail(T("Half rate"),Tone::Normal,value,nullptr);
                HelpMarker(T("every other frame, when the network runs"));
            }
        }
        if(s.enabled && s.scaleAuto && snapshot.dlss5Route && snapshot.dlss5EffectiveScale>0.0f){
            char value[32]{},detail[96]{};
            snprintf(value,sizeof(value),"%.0f%%",snapshot.dlss5EffectiveScale);
            if(snapshot.dlss5GpuMs>=0.0f)snprintf(detail,sizeof(detail),T("%.1f of %.1f ms"),snapshot.dlss5GpuMs,snapshot.dlss5BudgetMs);
            else snprintf(detail,sizeof(detail),"%s",T("measuring"));
            StatusDetail(T("Auto scale"),Tone::Normal,value,detail);
        }
        if(snapshot.dlss5Route){
            char detail[160]{};
            snprintf(detail,sizeof(detail),T("%ux%u, network %ux%u, passes: %u"),snapshot.dlss5Width,snapshot.dlss5Height,
                snapshot.dlss5ExtentWidth,snapshot.dlss5ExtentHeight,snapshot.dlss5Passes);
            StatusDetail(T("Route"),Tone::Normal,snapshot.dlss5Route==2?T("Direct provider"):T("Native NGX"),detail);
        }
        ImGui::EndTable();
    }
    // Notes explaining the state above.
    if(snapshot.dlss5Pending)Note(Tone::Pending,"%s",T("Waiting for DLSS 5 GPU work to complete before applying settings."));
    // autoscale.1: why Resolution Scale Auto runs below the slider.
    if(s.enabled && s.scaleAuto && snapshot.dlss5Route && snapshot.dlss5EffectiveScale>0.0f){
        const float most=dlss5::auto_mode::kLevels[dlss5::auto_mode::CeilingLevel(s.scale)];
        if(snapshot.dlss5ScaleFloorHeld)
            Note(Tone::Dim,T("Auto Resolution Scale holds %.0f%%, its quality floor. DLSS 5 still costs more than its budget here, so the frame rate pays instead of detail."),
                snapshot.dlss5EffectiveScale);
        else if(snapshot.dlss5EffectiveScale<most-0.5f)
            Note(Tone::Dim,T("Auto Resolution Scale runs at %.0f%% of up to %.0f%% to keep DLSS 5 within its budget, and rises again with headroom."),
                snapshot.dlss5EffectiveScale,most);
    }
    if(snapshot.dlss5State==static_cast<uint32_t>(dlss5::State::eOtherMod))
        Note(Tone::Dim,"%s",T("Restart with other DLSS 5 add-ons disabled to give MFGUnlocker ownership of the provider."));
    if(snapshot.dlss5State==static_cast<uint32_t>(dlss5::State::eCreateFailed)
        || snapshot.dlss5State==static_cast<uint32_t>(dlss5::State::eEvaluateFailed))
        Note(Tone::Bad,"DLSSNR 0x%08X",snapshot.dlss5Result);
    // xemfgsa.5: XeMFG standalone's game chain carries no menu session, where the Present stage runs.
    if(present && xess_ui::XemfgRuns(gXessUi))
        Note(Tone::Pending,"%s",T("The Present stage does not run with XeMFG standalone. Choose Auto or an upscaling stage."));

    // Closed by default; ImGui keeps it open or closed for the session.
    if(ImGui::CollapsingHeader(l10n::Label("Advanced"))){
        Section(T("Look"));
        if(BeginGrid("##dlss5look")){
            ControlRow(T("Structure Intensity"));
            save|=Dlss5SliderSave(ImGui::SliderFloat("##dlss5structure",&s.structure,0.0f,dlss5::kMaxStrength,"%.2f"));
            HelpMarker(T("Local detail and structure enhancement.\nAbove 1 goes past the provider's usual range."));
            ControlRow(T("Tone Intensity"));
            save|=Dlss5SliderSave(ImGui::SliderFloat("##dlss5tone",&s.tone,0.0f,dlss5::kMaxStrength,"%.2f"));
            HelpMarker(T("Local tone and contrast enhancement.\nAbove 1 goes past the provider's usual range."));
            ControlRow(T("Character Mask"));
            save|=ImGui::Checkbox("##dlss5mask",&s.characterMask);
            HelpMarker(T("Detects skin and faces so that Faces & skin applies to them."));
            ImGui::EndTable();
        }

        // Passes 2 and later; their rows stay saved while Pass Count is 1.
        Section(T("Passes"));
        if(BeginGrid("##dlss5passes")){
            int passes=static_cast<int>(s.passes);
            ControlRow(T("Pass Count"));
            if(Dlss5SliderSave(ImGui::SliderInt("##dlss5passes",&passes,1,static_cast<int>(dlss5::kMaxPasses),"%d")))save=true;
            s.passes=static_cast<uint32_t>(std::clamp(passes,1,static_cast<int>(dlss5::kMaxPasses)));
            HelpMarker(T("Runs the network several times in a row. Every pass is a separate feature and costs GPU time and memory."));
            const char* const later[]{T("Same as pass 1"),T("Custom")};
            uint32_t custom=s.laterCustom?1u:0u;
            ControlRow(T("Later passes"));
            if(ChoiceCombo("##dlss5later",custom,later,2)){
                s.laterCustom=custom!=0;save=true;
                // Custom starts from pass 1's look, so switching changes nothing yet.
                if(s.laterCustom){s.laterModel=s.model;s.laterIntensity=s.intensity;s.laterStructure=s.structure;s.laterTone=s.tone;s.laterCharacter=s.character;}
            }
            HelpMarker(T("Passes 2 and later refine the previous pass's output. Custom gives them their own Model and strengths.\n"
                "Tone adds up from pass to pass, so later passes usually want less of it."));
            if(s.laterCustom){
                ImGui::BeginDisabled(s.passes<2);
                const char* const models[]{T("Model A"),T("Model B"),T("Model C")};
                ControlRow(T("Model"));
                save|=ChoiceCombo("##dlss5latermodel",s.laterModel,models,3);
                ControlRow(T("Intensity"));
                save|=Dlss5SliderSave(ImGui::SliderFloat("##dlss5laterintensity",&s.laterIntensity,0.0f,1.0f,"%.2f"));
                ControlRow(T("Structure Intensity"));
                save|=Dlss5SliderSave(ImGui::SliderFloat("##dlss5laterstructure",&s.laterStructure,0.0f,dlss5::kMaxStrength,"%.2f"));
                ControlRow(T("Tone Intensity"));
                save|=Dlss5SliderSave(ImGui::SliderFloat("##dlss5latertone",&s.laterTone,0.0f,dlss5::kMaxStrength,"%.2f"));
                ControlRow(T("Faces & skin"));
                save|=FacesControl("dlss5latercharacter",s.laterCharacter,s.laterStructure,s.characterMask);
                ImGui::EndDisabled();
            }
            ImGui::EndTable();
        }
        if(s.laterCustom && s.passes<2)Note(Tone::Dim,"%s",T("Later-pass settings apply when Pass Count is 2 or more."));

        // Applied once to the last pass's result, in our decode pass.
        Section(T("Blend"));
        if(BeginGrid("##dlss5blend")){
            ControlRow(T("Blend strength"));
            save|=Dlss5SliderSave(ImGui::SliderFloat("##dlss5blendstrength",&s.blend,0.0f,dlss5::kMaxBlend,"%.2f"));
            HelpMarker(T("Scales DLSS 5's change: 0 keeps the game's image, 1 is DLSS 5's result, above 1 exaggerates it."));
            ControlRow(T("Color strength"));
            save|=Dlss5SliderSave(ImGui::SliderFloat("##dlss5colorstrength",&s.color,0.0f,dlss5::kMaxColor,"%.2f"));
            HelpMarker(T("0 keeps the game's colors and takes only DLSS 5's lighting and detail. 1 is DLSS 5's color; above 1 strengthens its color change."));
            ControlRow(T("Highlight guard"));
            save|=ImGui::Checkbox("##dlss5guard",&s.highlightGuard);
            HelpMarker(T("Limits how far DLSS 5 may brighten a pixel, as a multiple of its original brightness, so lights do not turn into bright speckles."));
            ControlRow(T("Brightening limit"));
            ImGui::BeginDisabled(!s.highlightGuard);
            save|=Dlss5SliderSave(ImGui::SliderFloat("##dlss5guardlimit",&s.highlightLimit,dlss5::kMinHighlightLimit,dlss5::kMaxHighlightLimit,"%.1fx"));
            ImGui::EndDisabled();
            ControlRow(T("Dark halo reduction"));
            save|=Dlss5SliderSave(ImGui::SliderFloat("##dlss5halodark",&s.haloDark,0.0f,1.0f,"%.2f"));
            HelpMarker(T("Removes thin dark outlines DLSS 5 adds beside bright edges and keeps its broad lighting change. Can lighten real shadow edges."));
            ControlRow(T("Bright halo reduction"));
            save|=Dlss5SliderSave(ImGui::SliderFloat("##dlss5halobright",&s.haloBright,0.0f,1.0f,"%.2f"));
            HelpMarker(T("Removes thin bright outlines DLSS 5 adds beside dark edges."));
            int radius=static_cast<int>(s.haloRadius);
            ControlRow(T("Halo radius"));
            ImGui::BeginDisabled(s.haloDark<=0.0f && s.haloBright<=0.0f);
            if(Dlss5SliderSave(ImGui::SliderInt("##dlss5haloradius",&radius,static_cast<int>(dlss5::kMinHaloRadius),static_cast<int>(dlss5::kMaxHaloRadius),"%d")))save=true;
            s.haloRadius=static_cast<uint32_t>(std::clamp(radius,static_cast<int>(dlss5::kMinHaloRadius),static_cast<int>(dlss5::kMaxHaloRadius)));
            ImGui::EndDisabled();
            HelpMarker(T("How far from each pixel to look for the edge, in network pixels."));
            ImGui::EndTable();
        }

        Section(T("Scaling"));
        if(BeginGrid("##dlss5scaling")){
            const char* const domains[]{"scRGB-nl",T("Signed PQ"),T("Linear")};
            ControlRow(T("Scaling Domain"));
            save|=ChoiceCombo("##dlss5domain",s.domain,domains,3);
            HelpMarker(T("Signal space used for downsampling and upsampling."));
            const char* const downsample[]{T("Area"),T("Bilinear"),T("Nearest")};
            ControlRow(T("Downsample Filter"));
            save|=ChoiceCombo("##dlss5down",s.downsample,downsample,3);
            const char* const upsample[]{T("Bilinear"),T("Bicubic"),T("Nearest"),T("Edge-aware")};
            ControlRow(T("Upsample Filter"));
            save|=ChoiceCombo("##dlss5up",s.upsample,upsample,4);
            HelpMarker(T("Resolution Scale below 100: how the network's change is enlarged. Edge-aware keeps it from bleeding across edges."));
            const char* const supersample[]{T("Bilinear"),T("Area")};
            ControlRow(T("Supersampling Filter"));
            save|=ChoiceCombo("##dlss5supersample",s.supersample,supersample,2);
            HelpMarker(T("Resolution Scale above 100: how the network's larger image comes back to the game's size. Area averages every pixel it covers, without aliasing."));
            const char* const reconstruction[]{T("RGB residual"),T("Processed image"),T("Ratio")};
            ControlRow(T("Scale Reconstruction"));
            save|=ChoiceCombo("##dlss5reconstruction",s.reconstruction,reconstruction,3);
            HelpMarker(T("RGB residual adds the network's change to the full-resolution image.\n"
                "Processed image uses the upscaled network output directly.\n"
                "Ratio multiplies the full-resolution image by the network's change, so black stays black."));
            ImGui::EndTable();
        }

        Section(T("HDR and signal"));
        if(BeginGrid("##dlss5hdr")){
            const char* const encodings[]{T("Auto"),T("Linear BT.709"),"sRGB","HDR10","scRGB","scRGB-nl"};
            ControlRow(T("Encoding"));
            save|=ChoiceCombo("##dlss5encoding",s.encoding,encodings,6);
            HelpMarker(T("Auto treats the DLSS input and output as linear BT.709 and follows the swapchain format and color space at Present.\n"
                "Other choices override the encoding of the processed image."));
            const char* const signals[]{"sRGB",T("PQ SDR"),T("Linear SDR")};
            ControlRow(T("NR Signal Encoding"));
            save|=ChoiceCombo("##dlss5signal",s.signal,signals,3);
            HelpMarker(T("Encoding supplied to the network after the HDR-to-SDR bridge."));
            const bool explicitEncoding=s.encoding!=static_cast<uint32_t>(dlss5::Encoding::eAuto);
            // The effective value; a hand edit (explicit encodings only) overrides it.
            float white=dlss5::DiffuseWhite(s);
            ControlRow(T("Diffuse White"));
            ImGui::BeginDisabled(!explicitEncoding);
            const bool whiteEdited=ImGui::SliderFloat("##dlss5white",&white,dlss5::kMinDiffuseWhite,dlss5::kMaxDiffuseWhite,T("%.0f nits"));
            if(whiteEdited){s.diffuseWhite=white;s.diffuseWhiteOverride=true;}
            save|=Dlss5SliderSave(whiteEdited);
            ImGui::EndDisabled();
            HelpMarker(T("Reference white of the HDR-to-SDR bridge. Encoding Auto selects it; choose an explicit encoding to set it."));
            ControlRow(T("Highlight roll-off"));
            save|=Dlss5SliderSave(ImGui::SliderFloat("##dlss5knee",&s.knee,dlss5::kMinKnee,dlss5::kMaxKnee,"%.2f"));
            HelpMarker(T("HDR only: up to this share of diffuse white the network sees the image unchanged; brighter values are compressed into the rest. Lower gives highlights more room."));
            ControlRow(T("Exposure"));
            save|=Dlss5SliderSave(ImGui::SliderFloat("##dlss5exposure",&s.exposure,0.0f,dlss5::kMaxExposure,"%.2fx",ImGuiSliderFlags_Logarithmic));
            HelpMarker(T("Multiplies the exposure used for neural rendering, independent of the game's exposure."));
            const char* const exposureModes[]{T("Off"),T("On"),T("Inverse")};
            ControlRow(T("Use Exposure Value"));
            save|=ChoiceCombo("##dlss5exposuremode",s.exposureMode,exposureModes,3);
            HelpMarker(T("On applies the game's DLSS exposure texture before neural rendering; Inverse divides by it."));
            ControlRow(T("Adaptive exposure"));
            save|=ImGui::Checkbox("##dlss5adaptive",&s.adaptiveExposure);
            if(s.adaptiveExposure && snapshot.dlss5Route && snapshot.dlss5ExposureGain!=1.0f){
                ImGui::SameLine();ImGui::TextDisabled("x%.2f",snapshot.dlss5ExposureGain);
            }
            HelpMarker(T("Keeps the scene the network sees at a steady brightness in HDR games, so the same settings suit games with different exposure.\n"
                "The game's own brightness is unchanged. SDR color is left as it is."));
            ControlRow(T("Adaptive target"));
            ImGui::BeginDisabled(!s.adaptiveExposure);
            save|=Dlss5SliderSave(ImGui::SliderFloat("##dlss5adaptivetarget",&s.adaptiveTarget,dlss5::kMinAdaptiveTarget,dlss5::kMaxAdaptiveTarget,"%.2f"));
            ImGui::EndDisabled();
            HelpMarker(T("The average brightness Adaptive exposure gives the scene the network sees (0.18 is middle grey). Higher shows it a brighter scene."));
            ImGui::EndTable();
        }

        Section(T("Provider"));
        if(BeginGrid("##dlss5provider")){
            const char* const presets[]{T("Default"),T("Preset A"),T("Preset B")};
            ControlRow(T("Preset"));
            save|=ChoiceCombo("##dlss5preset",s.preset,presets,3);
            HelpMarker(T("The provider's render preset hint."));
            const char* const uiCorrection[]{T("Auto"),T("Off"),T("On")};
            ControlRow(T("UI Correction"));
            save|=ChoiceCombo("##dlss5ui",s.uiCorrection,uiCorrection,3);
            HelpMarker(T("Auto enables the provider's UI correction at Present, whose image contains the game's UI, and leaves it off for DLSS input and output.\n"
                "Off never uses it. On always does."));
            ImGui::EndTable();
        }

        // Boost's own setting; in place, disabled, wherever Boost is. Offered while
        // dlss5::kHalfRateOffered (halfgrid.1).
        if constexpr(dlss5::kHalfRateOffered){
            Section(T("Half rate"));
            if(BeginGrid("##dlss5boostadvanced")){
                const bool boostUnavailable=snapshot.graphicsApi==2u || (s.enabled && s.stage==static_cast<uint32_t>(dlss5::Stage::ePresent));
                const char* const tolerances[]{T("Strict"),T("Normal"),T("Loose")};
                ControlRow(T("Motion tolerance"));
                ImGui::BeginDisabled(boostUnavailable || !s.boost);
                save|=ChoiceCombo("##dlss5boosttolerance",s.boostTolerance,tolerances,3);
                ImGui::EndDisabled();
                HelpMarker(T("How much a surface may change between frames and still keep the carried fine detail.\n"
                    "Strict carries it on fewer moving surfaces, which keep the rebuilt look; Loose carries more detail."));
                ImGui::EndTable();
            }
        }

        // Only the Present hook method uses these; they stay saved otherwise.
        if(present){
            Section(T("Present"));
            if(BeginGrid("##dlss5present")){
                const char* const resources[]{T("Never"),T("Optional"),T("Required")};
                ControlRow(T("Use DLSS resources"));
                save|=ChoiceCombo("##dlss5resources",s.resources,resources,3);
                HelpMarker(T("Present only. Optional reuses the DLSS motion vectors and depth when available, otherwise neutral inputs.\n"
                    "Required skips Present processing without them. Never always uses neutral inputs."));
                ControlRow(T("Block Hudless"));
                save|=ImGui::Checkbox("##dlss5hudless",&s.blockHudless);
                HelpMarker(T("Present only: keeps the game's HUD-less image from DLSS frame generation, so generated frames match processed ones."));
                ImGui::EndTable();
            }
        }

        // Inspection, not part of the look; last, like diagnostics.
        Section(T("Inspect"));
        if(BeginGrid("##dlss5inspect")){
            const char* const views[]{T("Off"),T("Network input"),T("Network output"),T("Change (x8)")};
            ControlRow(T("Debug view"));
            save|=ChoiceCombo("##dlss5debugview",s.debugView,views,4);
            HelpMarker(T("Shows what the network is given and returns, or where DLSS 5 changes the image (grey: unchanged; lighter or darker: changed).\n"
                "With Split view, on the DLSS 5 side only."));
            ControlRow(T("Swap sides"));
            ImGui::BeginDisabled(!s.compare);
            save|=ImGui::Checkbox("##dlss5swap",&s.compareSwap);
            ImGui::EndDisabled();
            HelpMarker(T("Split view shows DLSS 5 on the left and the original on the right."));
            ImGui::EndTable();
        }

        if(ImGui::Button(T("Reset to defaults"))){
            dlss5::Settings defaults{};defaults.enabled=s.enabled;defaults.stage=s.stage;
            defaults.compare=s.compare;defaults.compareSplit=s.compareSplit;defaults.compareKey=s.compareKey;
            if(!(defaults==s)){s=defaults;save=true;}
        }
    }
    ImGui::EndDisabled();
    s=dlss5::Sanitized(s);
    if(save){
        gDlss5SaveFailed=!NativeFilePersistDlss5(s);
        gDlss5LastSave=GetTickCount64();gDlss5Dirty=false;
        if(gDlss5SaveFailed)s=before;
    }
}

const char* ReflexPendingMessage(uint32_t status)
{
    switch (static_cast<reflex_control::Status>(status))
    {
    case reflex_control::Status::eWaitingForModule: return T("Waiting for the game's Reflex plugin.");
    case reflex_control::Status::eUnsupportedModule: return T("The loaded Reflex plugin version is unsupported.");
    case reflex_control::Status::eHookUnavailable: return T("Reflex controls are unavailable for this plugin.");
    case reflex_control::Status::eIneligible: return T("The Reflex limit is unavailable for the current FG runtime.");
    case reflex_control::Status::eWaitingForGameOptions: return T("Waiting for game Reflex settings. Change the game's Reflex setting once to retry.");
    case reflex_control::Status::eWaitingForThread: return T("Waiting for the game's Reflex settings thread.");
    case reflex_control::Status::eWaitingForAvailability: return T("Waiting for Reflex availability.");
    case reflex_control::Status::eUnavailable: return T("Reflex is unavailable.");
    case reflex_control::Status::eRestorePending: return T("Restoring the game's Reflex limit...");
    case reflex_control::Status::eCallRejected: return T("The game rejected the Reflex limit.");
    case reflex_control::Status::eUnknownShape: return T("The game's Reflex settings cannot be safely replayed.");
    case reflex_control::Status::eStaleRoute: return T("The Reflex plugin changed. Restart the game.");
    case reflex_control::Status::eBusy: return T("Waiting for the next safe Reflex update.");
    default: return T("Waiting for the game to accept the Reflex limit.");
    }
}

// Set by the control rows; read by the status rows in the same frame.
bool gPresentationChangedThisFrame = false;

// V-Sync and Reflex limit rows for the DLSS Latency and display grid.
// xemfgsa.5: `xemfg` for the XeSS panel under XeMFG standalone, whose FPS limit XeLL applies to XeMFG's output
// (DLSS's Dynamic MFG never pauses it there).
void PresentationControlRows(const MfgUnlockReShadeSnapshot& snapshot, bool statusStale, bool xemfg = false)
{
    gPresentationChangedThisFrame = false;
    if (!BackendUsesNativeFile()) return;
    if (!statusStale)
    {
        gVsyncMode = static_cast<int>(snapshot.vsyncMode);
        gReflexFrameLimitFps = static_cast<int>(snapshot.reflexFrameLimitFps);
    }
    const int previousLimit = gReflexFrameLimitFps;
    const int previousVsync = gVsyncMode;
    bool changed = false;
    // Off submits the game's frames without V-Sync. A saved On from older
    // builds is not offered and behaves as Game.
    const char* const vsyncModes[] = {T("Game"), T("Off")};
    int vsyncChoice = gVsyncMode == 1 ? 1 : 0;
    ControlRow(T("V-Sync"));
    if (ImGui::Combo("##vsync", &vsyncChoice, vsyncModes, 2))
    {
        gVsyncMode = vsyncChoice;
        changed = true;
    }
    HelpMarker(T("Off submits the game's frames without V-Sync.\n"
        "NVIDIA Control Panel V-Sync settings take priority."));
    const bool dynamic = !xemfg && (snapshot.dynamicMode || snapshot.appliedDynamicMode);
    ControlRow(T("Reflex FPS limit"));
    ImGui::BeginDisabled(dynamic);
    bool limitEnabled = gReflexFrameLimitFps > 0;
    if (ImGui::Checkbox("##reflexlimit", &limitEnabled) && !dynamic)
    {
        gReflexFrameLimitFps = limitEnabled ? std::clamp(gReflexCustomLimitFps, 1, 1000) : 0;
        changed = true;
    }
    ImGui::SameLine();
    if (limitEnabled)
    {
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 6.0f);
        ImGui::DragInt("##reflexlimitfps", &gReflexCustomLimitFps, 1.0f, 1, 1000, "%d FPS");
        if (ImGui::IsItemDeactivatedAfterEdit() && !dynamic)
        {
            gReflexCustomLimitFps = std::clamp(gReflexCustomLimitFps, 1, 1000);
            gReflexFrameLimitFps = gReflexCustomLimitFps;
            changed = true;
        }
    }
    else
    {
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("%s", T("Off"));
    }
    ImGui::EndDisabled();
    HelpMarker(xemfg ? T("Caps the frame rate XeMFG presents.\nXeLL paces the game's frames to it.")
        : T("Caps the game's frame rate through Reflex.\n"
        "Paused while Dynamic MFG is on; the saved limit is kept."));
    if (changed)
    {
        const BOOL saved = NativeFilePersistPresentation(static_cast<uint32_t>(gVsyncMode),
            static_cast<uint32_t>(gReflexFrameLimitFps));
        gLastApplyAttempted = true;
        gLastApplyAccepted = saved != FALSE;
        gLastNativeConfigPersisted = saved != FALSE;
        gPresentationSaveFailed = !saved;
        if (!saved)
        {
            gReflexFrameLimitFps = previousLimit;
            gVsyncMode = previousVsync;
        }
    }
    gPresentationChangedThisFrame = changed;
}

void PresentationNotes(const MfgUnlockReShadeSnapshot& snapshot, bool xemfg = false)
{
    if (!BackendUsesNativeFile()) return;
    if (!xemfg && (snapshot.dynamicMode || snapshot.appliedDynamicMode) && gReflexFrameLimitFps > 0)
        Note(Tone::Dim, T("The Reflex limit is paused while Dynamic MFG is on. The saved limit is kept."));
    if (gPresentationSaveFailed)
        Note(Tone::Bad, T("The presentation settings could not be saved."));
}

const char* VulkanPresentModeName(uint32_t mode) noexcept
{
    switch (mode)
    {
    case vsync_control::kPresentModeImmediate: return "IMMEDIATE";
    case vsync_control::kPresentModeMailbox: return "MAILBOX";
    case vsync_control::kPresentModeFifo: return "FIFO";
    case vsync_control::kPresentModeFifoRelaxed: return "FIFO_RELAXED";
    }
    return "other";
}

// V-Sync readback for a Vulkan game (vsync_control.h): the present mode is
// fixed when the game creates its swapchain, so a change waits for its next
// one. The caller passes a current snapshot. Returns false when the snapshot
// describes no Vulkan presentation, leaving the DXGI rows to the caller.
bool VulkanVsyncRow(const MfgUnlockReShadeSnapshot& snapshot)
{
    using vsync_control::Failure;
    const auto failure = static_cast<Failure>(snapshot.vsyncFailure);
    if (!snapshot.vsyncVulkanSwapchain)
    {
        // A Vulkan game whose swapchain creation this module cannot see.
        if (snapshot.graphicsApi != 2u || snapshot.vsyncVulkanHook) return false;
        if (snapshot.vsyncMode == 1)
            StatusDetail(T("V-Sync"), Tone::Dim, T("Off not applied"), T("override unavailable"));
        return true;
    }
    if (snapshot.vsyncMode != 1)
    {
        // The live swapchain still carries an earlier Off.
        if (failure == Failure::eSwapchainRecreationPending)
            StatusDetail(T("V-Sync"), Tone::Pending, T("Game"), T("applies when the game recreates its swapchain"));
        return true;
    }
    char detail[64]{};
    if (snapshot.vsyncPresentationObserved && snapshot.vsyncOverrideApplied)
    {
        // Vulkan API mode names, as in the debug readback; not translated.
        snprintf(detail, sizeof(detail), "Vulkan %s -> %s",
            VulkanPresentModeName(snapshot.vsyncOriginalPresentMode),
            VulkanPresentModeName(snapshot.vsyncSubmittedPresentMode));
        StatusDetail(T("V-Sync"), Tone::Good, T("Off"), detail);
    }
    else if (snapshot.vsyncPresentationObserved)
        StatusDetail(T("V-Sync"), Tone::Good, T("Off"), T("the game already presents without V-Sync"));
    else if (failure == Failure::eSwapchainRecreationPending)
        StatusDetail(T("V-Sync"), Tone::Pending, T("Off requested"), T("applies when the game recreates its swapchain"));
    else if (failure == Failure::ePresentModeUnavailable)
        StatusDetail(T("V-Sync"), Tone::Dim, T("Off not applied"), T("not available on this GPU or driver"));
    else
        StatusDetail(T("V-Sync"), failure == Failure::eSwapchainCreateFailed ? Tone::Bad : Tone::Dim,
            T("Off not applied"), T("override unavailable"));
    return true;
}

// V-Sync and Reflex readback for the DLSS Status grid (and xemfgsa.5's XeSS panel under XeMFG standalone).
void PresentationStatusRows(const MfgUnlockReShadeSnapshot& snapshot, bool statusStale, bool xemfg = false)
{
    if (!BackendUsesNativeFile()) return;
    const bool changed = gPresentationChangedThisFrame;
    char detail[96]{};
    const bool vulkanRow = !statusStale && !changed
        && snapshot.vsyncMode == static_cast<uint32_t>(gVsyncMode) && VulkanVsyncRow(snapshot);
    if (!vulkanRow && gVsyncMode == 1)
    {
        const bool current = !statusStale && !changed && snapshot.vsyncMode == 1;
        if (current && snapshot.vsyncPresentationObserved && snapshot.vsyncOverrideApplied)
        {
            snprintf(detail, sizeof(detail), T("game interval %u submitted as 0"), snapshot.vsyncOriginalInterval);
            StatusDetail(T("V-Sync"), Tone::Good, T("Off"), detail);
        }
        else if (current && snapshot.vsyncPresentationObserved)
            StatusDetail(T("V-Sync"), Tone::Good, T("Off"), T("the game already presents without V-Sync"));
        else if (current && snapshot.vsyncFailure
                == static_cast<uint32_t>(vsync_control::Failure::ePresentFailed))
            StatusDetail(T("V-Sync"), Tone::Bad, T("Off not applied"), T("the game's last Present was not accepted"));
        else if (current)
            StatusDetail(T("V-Sync"), Tone::Pending, T("Off requested"), T("waiting for the game's swapchain"));
        else
            StatusRow(T("V-Sync"), Tone::Pending, "%s", T("Applying..."));
    }
    // xemfgsa.5: while XeMFG generates, the game's frames sleep in XeLL and XeLL holds the limit; otherwise they sleep
    // in the driver's Reflex as before.
    if (xemfg && gXessUi.xemfgGenerating && gReflexFrameLimitFps > 0)
    {
        char value[48]{};
        const auto& x = gXessUi;
        if (!statusStale && !changed && x.xemfgFpsLimited && x.xemfgFpsLimit == static_cast<uint32_t>(gReflexFrameLimitFps))
        {
            snprintf(value, sizeof(value), "%d FPS", gReflexFrameLimitFps);
            StatusDetail(T("Reflex limit"), Tone::Good, value, T("applied by XeLL to XeMFG's output"));
        }
        else
        {
            snprintf(value, sizeof(value), T("%d FPS requested"), gReflexFrameLimitFps);
            StatusDetail(T("Reflex limit"), Tone::Pending, value, T("applies with XeMFG's next frame"));
        }
        return;
    }
    const bool dynamic = !xemfg && (snapshot.dynamicMode || snapshot.appliedDynamicMode);
    if (gReflexFrameLimitFps > 0)
    {
        const uint32_t requestedUs = (1000000u + gReflexFrameLimitFps - 1u) / gReflexFrameLimitFps;
        char value[48]{};
        if (!dynamic && !statusStale && !changed && snapshot.reflexAppliedKnown && !snapshot.reflexLimitPending
            && !snapshot.reflexRestorePending && snapshot.reflexAppliedFrameLimitUs == requestedUs
            && snapshot.reflexStatus == static_cast<uint32_t>(reflex_control::Status::eApplied))
        {
            snprintf(value, sizeof(value), "%.2f FPS", 1000000.0 / snapshot.reflexAppliedFrameLimitUs);
            StatusDetail(T("Reflex limit"), Tone::Good, value, T("accepted"));
        }
        else
        {
            snprintf(value, sizeof(value), T("%d FPS requested"), gReflexFrameLimitFps);
            if (snapshot.reflexRestorePending)
                StatusDetail(T("Reflex limit"), Tone::Pending, value, T("restoring the game's Reflex limit"));
            else if (dynamic)
                StatusDetail(T("Reflex limit"), Tone::Dim, value, T("paused for Dynamic MFG"));
            else
                StatusDetail(T("Reflex limit"), Tone::Pending, value, statusStale
                    ? T("Reflex status is temporarily unavailable.") : ReflexPendingMessage(snapshot.reflexStatus));
        }
    }
    else if (snapshot.reflexRestorePending)
        StatusRow(T("Reflex limit"), Tone::Pending, "%s", T("Restoring the game's Reflex limit..."));
}

void DrawPresentationDebug(const MfgUnlockReShadeSnapshot& snapshot)
{
    ImGui::Text("V-Sync: requested=%u available=%u observed=%u applied=%u interval=%u->%u failure=%s",
        snapshot.vsyncMode, snapshot.vsyncControlAvailable, snapshot.vsyncPresentationObserved,
        snapshot.vsyncOverrideApplied, snapshot.vsyncOriginalInterval, snapshot.vsyncSubmittedInterval,
        vsync_control::FailureName(static_cast<vsync_control::Failure>(snapshot.vsyncFailure)));
    ImGui::Text("V-Sync Vulkan: swapchain=%u hook=%u presentMode=%s->%s",
        snapshot.vsyncVulkanSwapchain, snapshot.vsyncVulkanHook,
        VulkanPresentModeName(snapshot.vsyncOriginalPresentMode),
        VulkanPresentModeName(snapshot.vsyncSubmittedPresentMode));
    ImGui::Text("Reflex: available=%u accepted=%u limit=%u us pending=%u restore=%u status=%u result=%d",
        snapshot.reflexControlAvailable, snapshot.reflexAppliedKnown, snapshot.reflexAppliedFrameLimitUs,
        snapshot.reflexLimitPending, snapshot.reflexRestorePending, snapshot.reflexStatus,
        snapshot.reflexLastResult);
    ImGui::Text("Reflex module: %u.%u.%u generation=%llu hooks=0x%X",
        snapshot.reflexModuleVersionMajor, snapshot.reflexModuleVersionMinor,
        snapshot.reflexModuleVersionPatch, static_cast<unsigned long long>(snapshot.reflexModuleGeneration),
        snapshot.reflexHookMask);
}

// Witcher 3 only. The fallback is always prepared there; the game's own
// HairWorks / Path Traced Hair settings turn hair on and off.
void DrawWitcherHair()
{
    const auto status = witcher_dots::ReadSnapshot();
    if (!status.applicable) return;
    Section(T("Path traced hair"));
    const bool active = status.stage == witcher_dots::Stage::Active;
    if (BeginGrid("##hair"))
    {
        if (status.environmentOverride)
            StatusDetail(T("State"), Tone::Dim, T("Off"), "RTXMFG_WITCHER_DOTS=0");
        else
        {
            // witcher_dots::ActivityText as a value and a detail, e.g. "Active" + "tracing converted hair".
            const char* text = T("Unknown");
            const char* detail = "";
            switch (status.stage)
            {
            case witcher_dots::Stage::Disabled: text = T("Disabled"); break;
            case witcher_dots::Stage::WaitingForDevice: text = T("Waiting for early device"); break;
            case witcher_dots::Stage::Preparing: text = T("Preparing"); break;
            case witcher_dots::Stage::UnsupportedGame: text = T("Unsupported game build"); break;
            case witcher_dots::Stage::UnsupportedGpu: text = T("Unsupported GPU"); break;
            case witcher_dots::Stage::NativeLss: text = T("Native LSS"); break;
            case witcher_dots::Stage::Failed: text = T("Unavailable"); break;
            case witcher_dots::Stage::UnsupportedDriver: text = T("Unsupported driver"); break;
            case witcher_dots::Stage::Active:
                if (!status.gameHairTraced) { text = T("Off"); detail = T("game Path Traced Hair setting"); }
                else if (status.lastHairAgeMs < 2000) { text = T("Active"); detail = T("tracing converted hair"); }
                else if (status.builds) { text = T("Idle"); detail = T("no hair traced in the last 2 s"); }
                else { text = T("Ready"); detail = T("waiting for game hair"); }
                break;
            }
            Tone tone = Tone::Normal;
            switch (status.stage)
            {
            case witcher_dots::Stage::Active:
                tone = !status.gameHairTraced ? Tone::Dim : status.lastHairAgeMs < 2000 ? Tone::Good : Tone::Normal;
                break;
            case witcher_dots::Stage::Disabled: tone = Tone::Dim; break;
            case witcher_dots::Stage::Failed: tone = Tone::Bad; break;
            case witcher_dots::Stage::NativeLss: tone = Tone::Normal; break;
            default: tone = Tone::Pending; break;
            }
            StatusDetail(T("State"), tone, text, detail);
            HelpMarker(T("Turn it on or off with HairWorks and Path Traced Hair in the game's graphics settings."));
            if (active || status.builds)
            {
                StatusRow(T("Traced hair"), Tone::Normal, "%u", status.lastHairAgeMs < 2000 ? status.hairInstances : 0u);
                char updates[64]{};
                snprintf(updates, sizeof(updates), T("%s updates"), Grouped(status.updates).c_str());
                StatusDetail(T("Builds"), Tone::Normal, Grouped(status.builds).c_str(), updates);
            }
        }
        ImGui::EndTable();
    }
    if (!status.environmentOverride && status.reason[0])
        Note(active ? Tone::Dim : Tone::Pending, "%s", status.reason);
}

void DrawWitcherHairDiagnostics()
{
    const auto status = witcher_dots::ReadSnapshot();
    if (!status.applicable || status.environmentOverride) return;
    if (status.stage != witcher_dots::Stage::Active && !status.builds) return;
    if (!ImGui::CollapsingHeader(l10n::Label("Hair diagnostics"))) return;
    constexpr double mib = 1024.0 * 1024.0, gib = 1024.0 * 1024.0 * 1024.0;
    // DOTS time per second by hook, sampled once a second.
    static uint64_t sampleTick = 0, sampleBuild = 0, sampleCopy = 0, sampleCalls = 0, sampleInstances = 0,
        sampleWait = 0, sampleHeld = 0;
    static double buildMs = 0, copyMs = 0, callsPerSecond = 0, instancesPerSecond = 0, waitMs = 0, heldMs = 0;
    const uint64_t tick = GetTickCount64();
    if (!sampleTick || tick - sampleTick >= 1000)
    {
        if (sampleTick)
        {
            const double seconds = (tick - sampleTick) / 1000.0;
            buildMs = (status.buildMicroseconds - sampleBuild) / 1000.0 / seconds;
            copyMs = (status.copyMicroseconds - sampleCopy) / 1000.0 / seconds;
            callsPerSecond = (status.copyCalls - sampleCalls) / seconds;
            instancesPerSecond = (status.instancesScanned - sampleInstances) / seconds;
            waitMs = (status.buildLockWaitMicroseconds - sampleWait) / 1000.0 / seconds;
            heldMs = (status.buildLockHeldMicroseconds - sampleHeld) / 1000.0 / seconds;
        }
        sampleTick = tick; sampleBuild = status.buildMicroseconds; sampleCopy = status.copyMicroseconds;
        sampleWait = status.buildLockWaitMicroseconds; sampleHeld = status.buildLockHeldMicroseconds;
        sampleCalls = status.copyCalls; sampleInstances = status.instancesScanned;
    }
    char value[64]{}, detail[128]{};
    ImGui::TextDisabled("Memory");
    if (BeginGrid("##hairmemory"))
    {
        StatusRow("Vertex pool", Tone::Normal, "%.1f MiB", status.geometryBytes / mib);
        StatusRow("Hair BLAS", Tone::Normal, "%.0f MiB", status.hairBlasBytes / mib);
        StatusRow("Scratch", Tone::Normal, "%.0f MiB", status.hairScratchBytes / mib);
        if (status.memoryKnown)
        {
            snprintf(value, sizeof(value), "%.2f of %.2f GiB", status.vramUsage / gib, status.vramBudget / gib);
            snprintf(detail, sizeof(detail), "shared %.2f GiB", status.sharedUsage / gib);
            StatusDetail("Game VRAM", Tone::Normal, value, detail);
        }
        ImGui::EndTable();
    }
    ImGui::TextDisabled("CPU per second");
    if (BeginGrid("##haircpu"))
    {
        snprintf(value, sizeof(value), "%.2f ms", buildMs);
        snprintf(detail, sizeof(detail), "lock wait %.2f, under lock %.2f, other %.2f ms",
            waitMs, heldMs, buildMs > waitMs + heldMs ? buildMs - waitMs - heldMs : 0.0);
        StatusDetail("Hair builds", Tone::Normal, value, detail);
        snprintf(value, sizeof(value), "%.2f ms", copyMs);
        snprintf(detail, sizeof(detail), "%.0f calls, %.0fk instances", callsPerSecond, instancesPerSecond / 1000.0);
        StatusDetail("Instance copies", Tone::Normal, value, detail);
        ImGui::EndTable();
    }
    ImGui::TextDisabled("Counters");
    if (BeginGrid("##haircounters"))
    {
        StatusRow("Prebuilds", Tone::Normal, "%s", Grouped(status.prebuilds).c_str());
        StatusRow("Shader libraries", Tone::Normal, "%s", Grouped(status.shaderLibraries).c_str());
        StatusRow("Instance copies", Tone::Normal, "%s", Grouped(status.instanceCopies).c_str());
        snprintf(detail, sizeof(detail), "%s released", Grouped(status.evictions).c_str());
        snprintf(value, sizeof(value), "%u", status.liveOwners);
        StatusDetail("Live hair owners", Tone::Normal, value, detail);
        StatusRow("Pool", Tone::Normal, "%s allocated, %s released, %s returned",
            Grouped(status.poolAllocations).c_str(), Grouped(status.poolReleases).c_str(),
            Grouped(status.poolReturns).c_str());
        StatusRow("Full rebuilds", Tone::Normal, "%s", Grouped(status.fullRebuilds).c_str());
        StatusRow("Fallback checks", Tone::Normal, "%s", Grouped(status.rejected).c_str());
        ImGui::EndTable();
    }
    bool report = status.crashReportRequested;
    static bool reportSaveFailed = false;
    if (ImGui::Checkbox("GPU crash report, next launch", &report))
        reportSaveFailed = !witcher_dots::SetCrashReportRequested(report);
    HelpMarker("Enables D3D12 DRED breadcrumbs for the game's device. If the GPU crashes, the report is written to\n"
        "RTXMFG-witcher3-device-removed-<pid>.txt in the game folder (bin\\x64_dx12), or to %TEMP% if that\n"
        "folder cannot be written. Costs a little GPU time while enabled.");
    if (reportSaveFailed) Note(Tone::Bad, "Could not save the crash report setting.");
    if (status.crashReportArmed) Note(Tone::Good, "Crash report armed for this session.");
    else if (status.crashReportRequested) Note(Tone::Pending, "Crash report starts with the next launch.");
}

constexpr uint32_t kIntelVendor = 0x8086;
constexpr uint32_t kNvidiaVendor = 0x10de;

const char* VendorName(uint32_t vendor) noexcept
{
    switch (vendor)
    {
    case kNvidiaVendor: return "NVIDIA";
    case 0x1002: return "AMD";
    case kIntelVendor: return "Intel";
    default: return T("Unknown");
    }
}

uint32_t GameGpuVendor() noexcept
{
    return xess_ui::Vendor(gXessUi);
}

// Which frame generation the game is using decides which settings are shown.
const char* ActiveFrameGenerationName() noexcept
{
    const auto& active = gXessUi.activeFrameGeneration;
    return active == "xess" ? "XeSS (Intel)" : active == "dlss" ? "DLSS (NVIDIA)" : T("Off");
}

bool XemfgAvailable() noexcept;

// Shown under the frame generation controls until the game turns frame
// generation on. Returns whether it was shown.
bool FrameGenerationOffNote(const MfgUnlockReShadeSnapshot& snapshot)
{
    if (gXessUi.activeFrameGeneration != "none") return false;
    const bool dlssOffered = snapshot.setOptionsSeen || snapshot.activeWrapperObserved;
    const bool xessOffered = gXessUi.hooksActive != FALSE;
    if (dlssOffered && xessOffered)
        Note(Tone::Pending, T("Turn on frame generation in the game's settings (DLSS or XeSS)."));
    else if (dlssOffered)
        Note(Tone::Pending, T("Turn on frame generation in the game's settings (DLSS)."));
    else if (xessOffered)
        Note(Tone::Pending, T("Turn on frame generation in the game's settings (XeSS)."));
    // xemfghint.1: a game with neither (Enshrouded) gets frames from XeMFG standalone only.
    else if (XemfgAvailable() && !gXessUi.xemfgStandalone)
        Note(Tone::Pending, T("Turn on DLSS or XeSS frame generation in the game's settings. "
            "If the game has none, turn on XeMFG standalone."));
    else
        Note(Tone::Pending, T("Turn on DLSS or XeSS frame generation in the game's settings."));
    return true;
}

// Game FG and GPU rows; familyDetail adds the DLSS GPU family.
void GameRows(const MfgUnlockReShadeSnapshot& snapshot, bool familyDetail)
{
    const bool off = gXessUi.activeFrameGeneration == "none";
    StatusRow(T("Game FG"), off ? Tone::Dim : Tone::Normal, "%s", ActiveFrameGenerationName());
    const uint32_t vendor = GameGpuVendor();
    const std::string gpu = !vendor ? std::string() : gXessUi.adapter.empty()
        ? std::string(VendorName(vendor)) : gXessUi.adapter;
    const char* family = !familyDetail ? "" : snapshot.gpuFamily == 1u ? "Ada (RTX 40)"
        : snapshot.gpuFamily == 2u ? "Ampere (RTX 30)" : snapshot.gpuFamily == 4u ? "Turing (RTX 20)"
        : snapshot.gpuFamily == 5u ? "Blackwell (RTX 50)" : "";
    if (!gpu.empty())
        StatusDetail(T("GPU"), Tone::Normal, gpu.c_str(), family);
    else if (familyDetail && family[0])
        StatusRow(T("GPU"), Tone::Normal, "%s", family);
    else if (familyDetail)
        StatusRow(T("GPU"), Tone::Pending, "%s", T("Waiting for a supported game GPU"));
}

// GPU name override (gpuname.1): for games that remove their DLSS Frame Generation
// option on RTX 30. Saved beside the game; applies at the next game start. Shown on
// RTX 30, and whenever it is saved or active so it can always be turned off.
struct GpuNameView { bool loaded = false; bool saved = false; bool saveFailed = false; };
GpuNameView gGpuName;

bool GpuNameShown() noexcept
{
    if (!gGpuName.loaded)
    {
        gGpuName.saved = gpu_name_override::SavedOn();
        gGpuName.loaded = true;
    }
    return UiAmpere() || gGpuName.saved || gpu_name_override::Read().enabled;
}

void GpuNameControlRow()
{
    if (!GpuNameShown()) return;
    ControlRow(T("Game FG option"));
    bool unlock = gGpuName.saved;
    if (ImGui::Checkbox(l10n::Label("Unlock"), &unlock))
    {
        gGpuName.saveFailed = !gpu_name_override::Save(unlock);
        if (!gGpuName.saveFailed) gGpuName.saved = unlock;
    }
    std::string gpuHint = T("For games that hide DLSS Frame Generation on RTX 30 cards, such as Wuthering Waves.\n"
        "The game then sees an NVIDIA GeForce RTX 4090. DLSS, Streamline, the driver and MFG Unlocker\n"
        "keep using your real GPU. The game's recommended settings may assume an RTX 4090.\n"
        "Online games with anti cheat may treat any mod as a risk. Applies after a game restart.");
    if (gGpuFamily.load(std::memory_order_acquire) == 4u)
    {
        const auto at = gpuHint.find("RTX 30");
        if (at != std::string::npos) gpuHint.replace(at, 6, "RTX 20");
    }
    HelpMarker(gpuHint.c_str());
}

void GpuNameNotes()
{
    if (!GpuNameShown()) return;
    const auto state = gpu_name_override::Read();
    if (gGpuName.saveFailed)
        Note(Tone::Bad, T("The Game FG option could not be saved."));
    else if (gGpuName.saved != state.enabled)
        Note(Tone::Pending, "%s", gGpuName.saved ? T("Restart the game to unlock its Frame Generation option.")
                                                 : T("Restart the game to report your real GPU again."));
    else if (state.enabled && !state.armed)
        Note(Tone::Bad, T("The Game FG option could not be applied this session. See the log."));
    else if (state.enabled && state.reported)
        Note(Tone::Dim, T("The game sees an NVIDIA GeForce RTX 4090 this session."));
    else if (state.enabled && state.unchanged)
        Note(Tone::Dim, T("This GPU is not an RTX 30 card; the game sees its real name."));
}

// XeSS frame generation: status only. beta.8: XeSS MFG runs under OptiScaler, whose menu owns its
// controls (the frame generation ratio and the MFG model). Without OptiScaler the game's XeSS frame
// generation stays at Intel's 2x and RTXMFG leaves it untouched.
// xemfgsa.1: XeMFG standalone, MFG Unlocker as the XeFG host without OptiScaler (OptiScaler's XeFG output runs XeMFG
// under OptiScaler). Its rows sit in the frame generation grid of both panels: switched on, the XeSS panel shows on
// every GPU (xess_ui::SettingsSection).
bool gXemfgSaveFailed = false;

bool XemfgAvailable() noexcept
{
    return BackendUsesNativeFile() && !gXessUi.xemfgState.empty() && !gXessUi.optiScaler;
}

// xemfgsa.6: the last custom Dynamic target, kept while "Display refresh rate" is ticked.
int gXemfgCustomTargetFps = 120;

void XemfgControlRows()
{
    auto& x = gXessUi;
    auto wanted = x;
    bool on = x.xemfgStandalone != FALSE;
    int index = static_cast<int>(std::clamp<uint32_t>(x.xemfgSavedFrames ? x.xemfgSavedFrames : 2, 2, 6)) - 2;
    bool changed = false;
    ControlRow(T("XeMFG standalone"));
    changed |= ImGui::Checkbox("##xemfgstandalone", &on);
    HelpMarker(T("MFG Unlocker runs XeSS multi-frame generation itself, without OptiScaler, from the inputs of the "
        "game's DLSS frame generation or of its DLSS, XeSS or FSR 3.1 upscaling. It replaces the game's own frame "
        "generation.\nSwitching it on or off applies after a restart."));
    if (on)
    {
        static const char* modes[] = {"2x", "3x", "4x", "5x", "6x"};
        // xemfgsa.6: Dynamic MFG, our OptiScaler XeFG output's (xess/xemfg_dynamic.h), up to the frames set.
        bool dynamic = x.xemfgDynamic != FALSE;
        const char* dynamicLabel = T("Dynamic");
        ControlRow(T("MFG mode"));
        if (ImGui::BeginCombo("##xemfgmode", dynamic ? dynamicLabel : modes[index]))
        {
            for (int i = 0; i < IM_ARRAYSIZE(modes); ++i)
                if (ImGui::Selectable(modes[i], !dynamic && index == i)) { index = i; dynamic = false; changed = true; }
            if (ImGui::Selectable(dynamicLabel, dynamic)) { dynamic = true; changed = true; }
            ImGui::EndCombo();
        }
        HelpMarker(T("Dynamic changes how many frames XeMFG generates every game frame, so its frame rate follows the "
            "target, up to the frames set. The target is not a limit."));
        wanted.xemfgDynamic = dynamic ? TRUE : FALSE;
        if (dynamic)
        {
            ControlRow(T("Up to"));
            changed |= ImGui::Combo("##xemfgmost", &index, modes, IM_ARRAYSIZE(modes));
            bool automatic = x.xemfgTargetFps == 0;
            if (x.xemfgTargetFps) gXemfgCustomTargetFps = static_cast<int>(x.xemfgTargetFps);
            ControlRow(T("Dynamic target"));
            if (ImGui::Checkbox(l10n::Label("Display refresh rate"), &automatic)) changed = true;
            HelpMarker(T("The FPS limit when one is set, otherwise just under the display's refresh rate."));
            if (!automatic)
            {
                ControlRow(T("Target FPS"));
                ImGui::DragInt("##xemfgtarget", &gXemfgCustomTargetFps, 1.0f, 1, 1000, "%d FPS");
                if (ImGui::IsItemDeactivatedAfterEdit()) changed = true;
            }
            gXemfgCustomTargetFps = std::clamp(gXemfgCustomTargetFps, 1, 1000);
            wanted.xemfgTargetFps = automatic ? 0u : static_cast<uint32_t>(gXemfgCustomTargetFps);
            // The controller's rounding values: 0 at most, 1 nearest, 2 at least.
            const char* roundings[] = {T("Closest"), T("At least"), T("At most")};
            static constexpr uint32_t kRounding[] = {1, 2, 0};
            int rounding = x.xemfgRounding == 2 ? 1 : x.xemfgRounding == 0 ? 2 : 0;
            ImGui::BeginDisabled(x.xemfgTargetAsCap != FALSE);
            ControlRow(T("Rounding"));
            if (ImGui::Combo("##xemfgrounding", &rounding, roundings, IM_ARRAYSIZE(roundings))) changed = true;
            HelpMarker(T("Closest: the frame rate closest to the target. At least: the fewest frames that reach it. "
                "At most: the most frames that stay under it."));
            wanted.xemfgRounding = kRounding[std::clamp(rounding, 0, 2)];
            bool allow1x = x.xemfgAllow1x != FALSE;
            ControlRow(T("Allow 1X"));
            if (ImGui::Checkbox("##xemfgallow1x", &allow1x)) changed = true;
            HelpMarker(T("Lets Dynamic turn frame generation off when the game's own frames come nearest the target."));
            wanted.xemfgAllow1x = allow1x ? TRUE : FALSE;
            ImGui::EndDisabled();
            bool cap = x.xemfgTargetAsCap != FALSE;
            ControlRow(T("Use target as cap"));
            if (ImGui::Checkbox("##xemfgcap", &cap)) changed = true;
            HelpMarker(T("Holds XeMFG's frame rate at the target with the FPS limit and uses the fewest frames that "
                "reach it, never 1X."));
            wanted.xemfgTargetAsCap = cap ? TRUE : FALSE;
        }
    }
    if (changed)
    {
        gXemfgSaveFailed = !NativeFilePersistXemfg(on, static_cast<uint32_t>(index + 2), wanted);
        if (!gXemfgSaveFailed)
        {
            // Shown at once; the next status read confirms it.
            x = wanted;
            x.xemfgStandalone = on ? TRUE : FALSE;
            x.xemfgSavedFrames = static_cast<uint32_t>(index + 2);
        }
    }
}

// alpha.15 (B4-12): after a failed runtime search, where the pair belongs (the real folders, the game's EXE folder
// named, which Unreal games keep in Binaries\Win64), what was missing, and the restart the once-per-launch search needs.
void XemfgRuntimeNotes()
{
    const auto& x = gXessUi;
    const std::string& why = x.xemfgReason;
    if (why == "runtime_path_too_long")
    {
        Note(Tone::Bad, T("The game's folder path is too long (260 characters or more) for XeMFG to search it."));
        return;
    }
    if (x.xemfgRuntimeSearch.empty())
        Note(Tone::Bad, T("XeMFG needs libxess_fg.dll and libxell.dll in an RTXMFG-XeSS folder beside MFG Unlocker."));
    else
        Note(Tone::Bad, T("XeMFG needs libxess_fg.dll and libxell.dll together in %s (or beside the game's exe in %s). "
            "Restart the game after adding them."), x.xemfgRuntimeSearch.c_str(), x.xemfgRuntimeGame.c_str());
    if (why == "runtime_missing_libxess_fg") Note(Tone::Bad, T("Only libxell.dll was found."));
    else if (why == "runtime_missing_libxell") Note(Tone::Bad, T("Only libxess_fg.dll was found."));
    else if (why == "runtime_split") Note(Tone::Bad, T("The two files are in different folders."));
    else if (why == "runtime_misnamed" && !x.xemfgRuntimeMisnamed.empty())
        Note(Tone::Bad, T("Found %s, not %s: check the file's name and extension."), x.xemfgRuntimeMisnamed.c_str(),
            _strnicmp(x.xemfgRuntimeMisnamed.c_str(), "libxess_fg", 10) == 0 ? "libxess_fg.dll" : "libxell.dll");
}

// Notes for the rows above: what XeMFG waits for. Reasons come from the host (xess/d3d12_host.cpp, vk_host.cpp).
void XemfgNotes()
{
    const auto& x = gXessUi;
    if (gXemfgSaveFailed) Note(Tone::Bad, T("The XeMFG setting could not be saved."));
    if (!x.xemfgStandalone) return;
    const std::string& state = x.xemfgState;
    const std::string& why = x.xemfgReason;
    if (state == "restart")
        Note(Tone::Pending, T("Restart the game to start XeMFG."));
    else if (xess_runtime_search::SearchReason(why))
        XemfgRuntimeNotes(); // D3D12 reports these as unavailable, Vulkan as waiting: the same notes for both
    else if (state == "unavailable")
        Note(Tone::Bad, T("XeMFG could not start: %s"), why.c_str());
    else if (why == "game_frame_generation_off")
        Note(Tone::Pending, T("Turn on DLSS frame generation in the game's settings. XeMFG replaces it."));
    else if (why == "game_fsr_frame_generation")
        Note(Tone::Pending, T("Turn off the game's FSR frame generation. XeMFG does not run on top of it."));
    else if (why == "motion_vector_size")
        Note(Tone::Bad, T("The game's motion vectors are at display resolution. XeMFG cannot use them yet."));
    else if (state == "waiting")
        Note(Tone::Pending, T("Waiting for the game's DLSS, XeSS or FSR 3.1 upscaling, or its DLSS frame generation."));
}

void XemfgStatusRows()
{
    const auto& x = gXessUi;
    const std::string& state = x.xemfgState;
    if (!x.xemfgStandalone || state.empty() || state == "off" || state == "optiscaler") return;
    if (state == "active")
    {
        char value[64]{};
        // xemfgsa.6: Dynamic MFG's count (1x: frame generation off).
        if (x.xemfgDynamicActive) snprintf(value, sizeof(value), T("Dynamic, %ux"), x.xemfgFrames);
        else snprintf(value, sizeof(value), "%ux", x.xemfgFrames);
        StatusRow("XeMFG", Tone::Good, "%s", x.xemfgFrames >= 1 ? value : T("Active"));
        if (x.xemfgDynamicActive && x.xemfgDynamicTargetMilli)
        {
            snprintf(value, sizeof(value), "%.0f FPS", x.xemfgDynamicTargetMilli / 1000.0);
            if (x.xemfgDynamicCap) StatusDetail(T("Dynamic target"), Tone::Normal, value, T("held as cap"));
            else StatusRow(T("Dynamic target"), Tone::Normal, "%s", value);
        }
    }
    else if (state == "restart")
        StatusRow("XeMFG", Tone::Pending, "%s", T("Restart needed"));
    else if (state == "unavailable")
        StatusDetail("XeMFG", Tone::Bad, T("Unavailable"), x.xemfgReason.c_str());
    else
        StatusDetail("XeMFG", Tone::Pending, T("Waiting"), x.xemfgReason.c_str());
    const char* source = x.xemfgSource == "streamline" ? "Streamline DLSS-G" : x.xemfgSource == "dlss" ? "DLSS"
        : x.xemfgSource == "xess" ? "XeSS" : x.xemfgSource == "fsr" ? "FSR" : nullptr;
    if (source) StatusRow(T("Inputs"), Tone::Normal, "%s", source);
    // xemfgsa.5: the FPS row and its pin, as in the DLSS panel (DLSS-G's own counters stay at Off while XeMFG replaces
    // it): the game's frames and every frame XeFG presents.
    if (!xess_ui::XemfgRuns(x)) return;
    const char* pinHelp = T("Shows the real and XeMFG frame rates in the top-left corner\n"
        "while the menu is closed. Saved for the next launch.");
    if (x.xemfgGenerating && xess_ui::XemfgFpsCurrent(x))
        StatusRow("FPS", Tone::Normal, T("%.1f real, %.1f XeMFG"), x.xemfgBaseFpsMilli / 1000.0, x.xemfgOutputFpsMilli / 1000.0);
    else if (x.xemfgGenerating)
        StatusRow("FPS", Tone::Dim, "%s", T("Measuring..."));
    else if (gPinnedFps.load(std::memory_order_acquire))
        StatusRow("FPS", Tone::Dim, "%s", T("XeMFG off"));
    else
        return;
    PinnedFpsToggle(pinHelp);
}

void DrawXessPanel(const MfgUnlockReShadeSnapshot& snapshot, bool statusStale)
{
    const auto& x = gXessUi;
    static const char* models[] = {N_("MFG Unlocker multi-frame port"), N_("Intel 2x model")};

    Section(T("Frame generation"));
    if (XemfgAvailable())
    {
        BeginStaleDisabled(statusStale);
        if (BeginGrid("##xemfg"))
        {
            XemfgControlRows();
            ImGui::EndTable();
        }
        ImGui::EndDisabled();
        XemfgNotes();
    }
    // XeMFG standalone needs no frame generation of the game's own (only the game's DLSS switch, noted above).
    if (!x.xemfgStandalone) FrameGenerationOffNote(snapshot);
    if (!x.available)
        Note(Tone::Pending, T("XeSS frame generation support is unavailable in this build."));
    else if (!x.optiScaler && !x.xemfgStandalone)
        Note(Tone::Pending, T("XeSS multi-frame generation runs as XeMFG standalone, or through OptiScaler with XeFG "
            "as its frame generation output. Without either, the game's XeSS frame generation runs at Intel's standard 2x."));
    else if (x.optiScaler)
        Note(Tone::Normal, T("XeSS multi-frame generation runs through OptiScaler. Set the frame "
            "generation ratio and the MFG model in OptiScaler's menu."));
    const uint32_t vendor = GameGpuVendor();
    const bool dlssOffered = snapshot.setOptionsSeen || snapshot.activeWrapperObserved;
    if (vendor && vendor != kNvidiaVendor)
        Note(Tone::Dim, T("DLSS multi-frame generation needs an NVIDIA RTX GPU."));
    else if (dlssOffered && gXessUi.gpuVendorId == kNvidiaVendor && !xess_ui::DlssMfgFamily(snapshot.gpuFamily))
        Note(Tone::Dim, T("DLSS multi-frame generation is unavailable on this GPU."));

    // xemfgsa.5: XeMFG standalone paces the game's frames itself (XeLL): the DLSS panel's latency rows, with the FPS
    // limit XeLL applies to XeMFG's output. Otherwise V-Sync only (OptiScaler's menu owns its frame limit).
    const bool xemfgLatency = BackendUsesNativeFile() && x.xemfgStandalone && !x.optiScaler;
    int vsync = snapshot.vsyncMode == 1 ? 1 : 0;
    bool vsyncChanged = false;
    if (xemfgLatency)
    {
        Section(T("Latency and display"));
        BeginStaleDisabled(statusStale);
        if (BeginGrid("##xemfglatency"))
        {
            PresentationControlRows(snapshot, statusStale, true);
            ImGui::EndTable();
        }
        ImGui::EndDisabled();
        PresentationNotes(snapshot, true);
    }
    else
    {
        Section(T("Display"));
        BeginStaleDisabled(statusStale);
        if (BeginGrid("##xessdisplay"))
        {
            const char* vsyncModes[] = {T("Game"), T("Off")};
            ControlRow(T("V-Sync"));
            vsyncChanged = ImGui::Combo("##xessvsync", &vsync, vsyncModes, IM_ARRAYSIZE(vsyncModes));
            HelpMarker(T("Off submits the game's frames without V-Sync."));
            ImGui::EndTable();
        }
        if (vsyncChanged && !NativeFilePersistPresentation(static_cast<uint32_t>(vsync), snapshot.reflexFrameLimitFps))
            Note(Tone::Bad, T("The V-Sync setting could not be saved."));
        ImGui::EndDisabled();
    }

    // xemfgsa.5: the preset of the DLSS upscaling the game runs (XeMFG standalone often takes its inputs from it), as in
    // the DLSS panel.
    const bool dlssUpscaling = BackendUsesNativeFile() && (snapshot.dlssSr.running || snapshot.dlssRr.running);
    const bool rayReconstructionShown = dlssUpscaling && RayReconstructionShown(snapshot);
    const MfgUnlockDlssFeature& dlssView = rayReconstructionShown ? snapshot.dlssRr : snapshot.dlssSr;
    if (dlssUpscaling)
    {
        Section(rayReconstructionShown ? T("DLSS Ray Reconstruction") : T("DLSS Super Resolution"));
        BeginStaleDisabled(statusStale);
        if (BeginGrid("##xessdlsssr"))
        {
            DlssPresetControlRow(dlssView, rayReconstructionShown, statusStale);
            ImGui::EndTable();
        }
        ImGui::EndDisabled();
        DlssNotes(dlssView, rayReconstructionShown);
    }

    Section(T("Status"));
    char value[64]{}, detail[96]{};
    if (BeginGrid("##xessstatus"))
    {
        GameRows(snapshot, false);
        XemfgStatusRows();
        if (x.available && x.optiScaler)
        {
            if (xess_ui::AppliedKnown(x))
            {
                snprintf(value, sizeof(value), "%ux", x.applied);
                snprintf(detail, sizeof(detail), T("%u generated frames capacity"), x.capacity);
                StatusDetail(T("Applied"), Tone::Good, value, detail);
            }
            else if (x.initialized)
                StatusRow(T("Applied"), Tone::Pending, "%s", T("Not verified"));
            if (x.modelActive == 1 || x.modelActive == 2)
                StatusRow(T("Model"), Tone::Normal, "%s", T(models[x.modelActive - 1]));
            // tensorcore.1: the ported model's int8 layers on RTX 20/30/40/50 tensor cores (CUDA in XeFG's own command list).
            if (x.modelActive == 1 && x.tensorCores == 1)
            {
                if (x.tensorCoreLaunches)
                    StatusDetail(T("Tensor cores"), Tone::Good, T("On"), T("the ported model's int8 layers run as CUDA kernels"));
                else
                    StatusDetail(T("Tensor cores"), Tone::Pending, T("Ready"), T("no layer has run yet"));
            }
            else if (x.modelActive == 1 && x.tensorCores == 3)
                StatusDetail(T("Tensor cores"), Tone::Normal, T("Off"), T("XeFGTensorCores = off in OptiScaler.ini"));
            else if (x.modelActive == 1 && x.tensorCores == 2 && xess_ui::Vendor(x) == kNvidiaVendor)
                StatusDetail(T("Tensor cores"), Tone::Normal, T("Off"), T("not available on this GPU or driver"));
        }
        const bool vulkanRow = !xemfgLatency && !statusStale && !vsyncChanged && VulkanVsyncRow(snapshot);
        if (xemfgLatency)
            PresentationStatusRows(snapshot, statusStale, true);
        else if (!vulkanRow && vsync == 1)
        {
            const auto result = !statusStale && !vsyncChanged
                ? xess_ui::VsyncResult(snapshot.vsyncControlAvailable != FALSE,
                    snapshot.vsyncPresentationObserved != FALSE, snapshot.vsyncOverrideApplied != FALSE,
                    snapshot.vsyncOriginalInterval, snapshot.vsyncSubmittedInterval)
                : xess_ui::VsyncReadback::Pending;
            switch (result)
            {
            case xess_ui::VsyncReadback::Overridden:
                snprintf(detail, sizeof(detail), T("game interval %u submitted as 0"), snapshot.vsyncOriginalInterval);
                StatusDetail(T("V-Sync"), Tone::Good, T("Off"), detail);
                break;
            case xess_ui::VsyncReadback::AlreadyOff:
                StatusDetail(T("V-Sync"), Tone::Good, T("Off"), T("the game already presents without V-Sync"));
                break;
            default:
                StatusDetail(T("V-Sync"), Tone::Pending, T("Off requested"), T("presentation override is not verified"));
                break;
            }
        }
        if (dlssUpscaling) DlssStatusRows(dlssView, rayReconstructionShown);
        ImGui::EndTable();
    }
    if (x.available && x.optiScaler && !x.status.empty()) Note(Tone::Dim, "%s", x.status.c_str());
    DrawWitcherHair();

    Section(T("Advanced"));
    if (x.available && ImGui::CollapsingHeader(l10n::Label("XeSS diagnostics")))
    {
        ImGui::Text("OptiScaler %s | hooks %s | initialized %s | enabled %s",
            x.optiScaler ? "detected" : "not detected", x.hooksActive ? "active" : "waiting",
            x.initialized ? "yes" : "no", x.enabled ? "yes" : "no");
        ImGui::Text("Provider frames %u | model %u", x.framesPresented, x.modelActive);
        ImGui::Text("Tensor cores %u | launches %u | fallbacks %u%s%s", x.tensorCores, x.tensorCoreLaunches,
            x.tensorCoreFallbacks, x.tensorCoreReason.empty() ? "" : " | ", x.tensorCoreReason.c_str());
        if (!x.runtimeFailure.empty()) ImGui::TextWrapped("Runtime: %s", x.runtimeFailure.c_str());
    }
    DrawWitcherHairDiagnostics();
}

// OptiScaler's shared UI mode (the default again since alpha.19): its menu owns
// frame generation; this menu offers the DLSS SR/RR presets and DLSS 5.
void DrawOptiScalerShared(const MfgUnlockReShadeSnapshot& snapshot, bool statusStale)
{
    // Only the features in use: the preset of each running DLSS feature, and
    // DLSS 5 while its provider is present.
    const bool superResolution = snapshot.dlssSr.running != FALSE;
    const bool rayReconstruction = snapshot.dlssRr.running != FALSE;
    const bool neuralRendering = Dlss5TabShown(snapshot);
    if (!superResolution && !rayReconstruction && !neuralRendering)
    {
        Note(Tone::Pending, "%s", T("Waiting for DLSS"));
        return;
    }
    if (!BeginTabs("##mfgtabs")) return;
    if ((superResolution || rayReconstruction) && TabItem(T("DLSS presets")))
    {
        Note(Tone::Normal, "%s", T("Frame generation is set in OptiScaler's menu. This menu offers the DLSS presets and DLSS 5."));
        BeginStaleDisabled(statusStale);
        for (const bool rr : {false, true})
        {
            if (!(rr ? rayReconstruction : superResolution)) continue;
            const MfgUnlockDlssFeature& dlssView = rr ? snapshot.dlssRr : snapshot.dlssSr;
            ImGui::PushID(rr ? "rr" : "sr");
            Section(rr ? T("DLSS Ray Reconstruction") : T("DLSS Super Resolution"));
            if (BeginGrid("##optiscalerdlss"))
            {
                DlssPresetControlRow(dlssView, rr, statusStale);
                ImGui::EndTable();
            }
            DlssNotes(dlssView, rr);
            ImGui::PopID();
        }
        Section(T("Status"));
        if (BeginGrid("##optiscalerdlssstatus"))
        {
            if (superResolution) DlssStatusRows(snapshot.dlssSr, false);
            if (rayReconstruction) DlssStatusRows(snapshot.dlssRr, true);
            ImGui::EndTable();
        }
        ImGui::EndDisabled();
        ImGui::EndTabItem();
    }
    if (neuralRendering && TabItem(T("DLSS 5")))
    {
        DrawDlss5(snapshot, statusStale);
        ImGui::EndTabItem();
    }
    ImGui::EndTabBar();
}

void DrawSettings(reshade::api::effect_runtime* runtime)
{
    gOverlayVisitedThisFrame = true;
    gOverlayDockedThisFrame = ImGui::IsWindowDocked();
    if (gOverlayDockedThisFrame && !gDefaultDockInitialized)
    {
        gDefaultDockInitialized = true;
        reshade::set_config_value(runtime, kConfigSection,
            "DefaultDockInitialized", true);
    }

    MfgUnlockReShadeSnapshot snapshot{};
    bool haveSnapshot = BackendGetSnapshot(&snapshot) != FALSE;
    bool statusStale = false;
    bool nativeFile = false;
    nativeFile = BackendUsesNativeFile();
    if (nativeFile)
    {
        if (gLastNativeDisplayPath != gNativeStatusPath)
        {
            gLastNativeDisplayPath = gNativeStatusPath;
            gHaveNativeDisplay = false;
        }
        if (haveSnapshot)
        {
            gLastNativeDisplay = snapshot;
            gHaveNativeDisplay = true;
        }
        else
        {
            statusStale = true;
            if (gHaveNativeDisplay) { snapshot = gLastNativeDisplay; haveSnapshot = true; }
        }
    }
    if (!haveSnapshot)
    {
        if (nativeFile)
        {
            SetFooter(N_("Waiting for status"), Tone::Pending);
            Note(Tone::Pending, T("Waiting for current backend status. Saved settings are preserved."));
            return;
        }
        SetFooter(N_("Not connected"), Tone::Bad);
        Note(Tone::Bad, T("%s backend is not loaded."), MFG_PRODUCT);
        ImGui::TextWrapped(T("The integrated core is unavailable. Check the "
            "startup log and restart the game with one %s module."), MFG_PRODUCT);
        return;
    }
    if (nativeFile)
        SetFooter(statusStale ? N_("Paused") : N_("Live"), statusStale ? Tone::Pending : Tone::Good);
    else
        SetFooter("", Tone::Dim);
    if (nativeFile && snapshot.optiScalerUiOwner == 1u)
    {
        DrawOptiScalerShared(snapshot, statusStale);
        return;
    }

    {
        const bool dlssOffered = snapshot.setOptionsSeen || snapshot.activeWrapperObserved;
        if (xess_ui::SettingsSection(gXessUi, gLastGameFrameGeneration, dlssOffered, snapshot.gpuFamily)
            == xess_ui::Section::Xess)
        {
            // xemfgsa.5: DLSS 5 keeps its tab beside XeSS frame generation (XeMFG standalone included) while its
            // provider is present, as with OptiScaler.
            const bool neuralRendering = BackendUsesNativeFile() && Dlss5TabShown(snapshot);
            if (neuralRendering && BeginTabs("##mfgtabs"))
            {
                if (TabItem(T("Frame generation")))
                {
                    DrawXessPanel(snapshot, statusStale);
                    ImGui::EndTabItem();
                }
                if (TabItem(T("DLSS 5")))
                {
                    DrawDlss5(snapshot, statusStale);
                    ImGui::EndTabItem();
                }
                ImGui::EndTabBar();
            }
            else
                DrawXessPanel(snapshot, statusStale);
            return;
        }
    }
    if (snapshot.gpuFamily == 3u)
    {
        Note(Tone::Pending, T("The game changed graphics adapters. Restart the game to select a new GPU."));
        return;
    }

    MfgUnlockReShadeSnapshot presetView = snapshot;
    if (std::strcmp(snapshot.patchRoute, "remix") == 0)
    {
        Section(T("Frame generation"));
        Note(Tone::Normal, T("RTX Remix detected. Frame generation and its multiplier are managed "
            "in the RTX Remix menu."));
        BeginStaleDisabled(statusStale);
        if (BeginGrid("##remixfg"))
        {
            PresetControlRow(presetView, statusStale);
            ImGui::EndTable();
        }
        ImGui::EndDisabled();
        PresetNotes(presetView);
        Section(T("Status"));
        if (BeginGrid("##remixstatus"))
        {
            StatusRow(T("Provider patch"), snapshot.backportReadyAtCreate ? Tone::Good : Tone::Pending, "%s",
                snapshot.backportReadyAtCreate ? T("Ready at feature creation") : T("Waiting for frame generation"));
            PresetStatusRow(presetView);
            ImGui::EndTable();
        }
        return;
    }
    const int safeMaximumMultiplier = std::clamp(
        static_cast<int>(snapshot.safeMaximumMultiplier), 2, 6);
    const auto dynamicAvailability = ui_dynamic_mfg::Classify(snapshot);
    const bool dynamicCapabilityKnown = dynamicAvailability != ui_dynamic_mfg::Availability::Checking;
    const bool dynamicSupported = dynamicAvailability == ui_dynamic_mfg::Availability::Supported;
    const bool dynamicUnsupported = dynamicAvailability == ui_dynamic_mfg::Availability::Unavailable;
    const bool wrapperControlUnavailable = snapshot.activeWrapperObserved
        && snapshot.universalRouteFailure
            == static_cast<uint32_t>(universal_route_policy::Failure::eActiveWrapperUnpatched);
    bool settingsChanged = false;
    static const char* fixedModes[] = {
        "2x", "3x", "4x", "5x", "6x"
    };

    // A status protocol from another build makes every value below unreliable.
    const bool protocolMismatch = snapshot.statusProtocolVersion != MFG_STATUS_VERSION_NUMBER;
    if (protocolMismatch)
        Note(Tone::Bad, T("Restart with the matching single-module build. "
            "The current status protocol is older than this UI expects."));

    // DLSS 5 (standalone native control) has its own tab while its provider is present; without it, no tab bar.
    const bool tabs = BackendUsesNativeFile() && Dlss5TabShown(snapshot) && BeginTabs("##mfgtabs");
    if (!tabs || TabItem(T("Frame generation")))
    {
        Section(T("Frame generation"));
        // Keep an already-open popup and its appearance stable across a missed
        // read. Disabled input prevents cached capabilities from authorizing writes.
        BeginStaleDisabled(statusStale);
        if (BeginGrid("##dlssfg"))
        {
            // Capability can disappear in menus and during map transitions. Rendering
            // that state must not overwrite the user's persisted Dynamic request.
            if (UiAmpere())
            {
                gDynamicExperimental56 = false;
                gGeneratedOnlyDebug = false;
                static const char* ampereModes[]{N_("Off"), "2x FG", "3x MFG", "4x MFG",
                    N_("5x MFG (experimental)"), N_("6x MFG (experimental)")};
                const char* dynamicLabel = T(ui_dynamic_mfg::Label(dynamicAvailability));
                const char* selectedMode = gFollowGameMode ? T("Follow game")
                    : gDynamicMode ? dynamicLabel : T(ampereModes[std::clamp(gMultiplier, 1, 6) - 1]);
                ControlRow(T("Mode"));
                if (ImGui::BeginCombo("##framegeneration", selectedMode))
                {
                    if (ImGui::Selectable(T("Follow game"), gFollowGameMode))
                    { gFollowGameMode = true; gDynamicMode = false; settingsChanged = true; }
                    for (int value = 1; value <= 6; ++value)
                    {
                        ImGui::BeginDisabled(value > safeMaximumMultiplier);
                        if (ImGui::Selectable(T(ampereModes[value - 1]), !gFollowGameMode && !gDynamicMode && gMultiplier == value)
                            && value <= safeMaximumMultiplier)
                        { gFollowGameMode = false; gMultiplier = value; gDynamicMode = false; settingsChanged = true; }
                        ImGui::EndDisabled();
                    }
                    ImGui::BeginDisabled(!dynamicSupported);
                    if (ImGui::Selectable(dynamicLabel, gDynamicMode) && dynamicSupported)
                    { gFollowGameMode = false; gMultiplier = std::max(gMultiplier, 2); gDynamicMode = true; settingsChanged = true; }
                    ImGui::EndDisabled();
                    ImGui::EndCombo();
                }
            }
            else
            {
                const char* dynamicLabel = T(ui_dynamic_mfg::Label(dynamicAvailability));
                const char* currentMode = !snapshot.bridgeReady
                    ? T("Unavailable")
                    : gFollowGameMode
                    ? T("Follow game")
                    : gDynamicMode
                        ? dynamicLabel
                        : fixedModes[
                            std::clamp(gMultiplier, 2, 6) - 2];
                ControlRow(T("MFG mode"));
                ImGui::BeginDisabled(!snapshot.bridgeReady);
                if (ImGui::BeginCombo("##mfgmode", currentMode))
                {
                    if (ImGui::Selectable(T("Follow game"), gFollowGameMode))
                    {
                        gFollowGameMode = true;
                        gDynamicMode = false;
                        settingsChanged = true;
                    }
                    const int listedMaximum = snapshot.nvidiaPolicyCeilingMultiplier >= 2
                        && snapshot.nvidiaPolicyCeilingMultiplier <= 6
                        ? static_cast<int>(snapshot.nvidiaPolicyCeilingMultiplier) : safeMaximumMultiplier;
                    for (int multiplier = 2; multiplier <= listedMaximum;
                         ++multiplier)
                    {
                        const bool selected = !gFollowGameMode && !gDynamicMode
                            && gMultiplier == multiplier;
                        ImGui::BeginDisabled(multiplier > safeMaximumMultiplier);
                        if (ImGui::Selectable(fixedModes[multiplier - 2], selected)
                            && multiplier <= safeMaximumMultiplier)
                        {
                            gFollowGameMode = false;
                            gDynamicMode = false;
                            gMultiplier = multiplier;
                            settingsChanged = true;
                        }
                        ImGui::EndDisabled();
                    }
                    ImGui::BeginDisabled(!dynamicSupported);
                    if (ImGui::Selectable(dynamicLabel, gDynamicMode)
                        && dynamicSupported)
                    {
                        gFollowGameMode = false;
                        gDynamicMode = true;
                        settingsChanged = true;
                    }
                    ImGui::EndDisabled();
                    ImGui::EndCombo();
                }
                ImGui::EndDisabled();
            }
            HelpMarker(T("Follow game uses the game's own frame generation setting.\n"
                "Dynamic adjusts frame generation toward a target frame rate; it does not set a hard FPS limit."));
            if (!gFollowGameMode && gDynamicMode)
            {
                ImGui::BeginDisabled(!dynamicSupported);
                ControlRow(T("Dynamic target"));
                if (ImGui::Checkbox(l10n::Label("Display refresh rate"), &gDynamicLockToRefreshRate))
                    settingsChanged = true;
                if (!gDynamicLockToRefreshRate)
                {
                    ControlRow(T("Target FPS"));
                    ImGui::DragInt("##dynamictarget", &gDynamicCustomTargetFrameRate, 1.0f, 1, 1000, "%d FPS");
                    if (ImGui::IsItemDeactivatedAfterEdit())
                        settingsChanged = true;
                }
                ImGui::EndDisabled();
            }
            PresetControlRow(presetView, statusStale);
            GpuNameControlRow();
            // xemfgsa.1: switched on, the XeSS panel with XeMFG's own rows replaces this one.
            if (XemfgAvailable()) XemfgControlRows();
            ImGui::EndTable();
        }
        XemfgNotes();
        // Notes for the controls above: what frame generation waits for, then
        // how the selection applies. The game-off note replaces the generic waits.
        const bool gameFgOffNoted = FrameGenerationOffNote(snapshot);
        const uint32_t vendor = GameGpuVendor();
        if (vendor && vendor != kNvidiaVendor)
            Note(Tone::Dim, T("DLSS multi-frame generation needs an NVIDIA RTX GPU."));
        if (!protocolMismatch && !snapshot.bridgeReady)
        {
            if (wrapperControlUnavailable)
                Note(Tone::Pending, T("Frame generation stays under the game's control. "
                    "This wrapper has no verified MFG override support."));
            else if (snapshot.intervalValidSamples && !snapshot.setOptionsSeen)
                Note(Tone::Pending, T("DLSS-G activity observed; waiting for the game's FG control calls."));
            else if (!gameFgOffNoted)
                Note(Tone::Pending, "%s", snapshot.universalRouteFailure
                    == static_cast<uint32_t>(universal_route_policy::Failure::eAwaitingOptionsRequest)
                    ? T("Waiting for the game to request frame generation.")
                    : T("Waiting for an active DLSS-G pipeline."));
        }
        if (snapshot.streamlineRebuildRequired
            || snapshot.pipelineMayPredateDetour)
        {
            Note(Tone::Pending, T("Toggle frame generation off then on, or restart "
                "the game, to recreate the pipeline with this selection."));
        }
        if (!UiAmpere())
        {
            if (dynamicUnsupported)
                Note(Tone::Dim, T("Dynamic MFG is unavailable with the current limit or runtime. "
                    "Its full frame-generation range must fit the limit."));
            else if (!gFollowGameMode && gDynamicMode && !dynamicSupported)
                Note(Tone::Pending, T("Dynamic selection is saved; waiting for runtime support."));
            if (!gFollowGameMode && !gDynamicMode
                && (!snapshot.bridgeReady || gMultiplier > safeMaximumMultiplier))
                Note(Tone::Pending, T("Saved MFG request: %dx (unavailable in this session)"), gMultiplier);
        }
        if (gLastApplyAttempted && !gPresetSaveFailed
            && (!gLastApplyAccepted || !gLastNativeConfigPersisted))
        {
            if (gLastApplyAccepted)
                Note(Tone::Bad, T("Request accepted, but the configuration was not saved."));
            else
                Note(Tone::Bad, T("The selection could not be applied or saved."));
        }
        PresetNotes(presetView);
        GpuNameNotes();

        Section(T("Latency and display"));
        if (BeginGrid("##dlsslatency"))
        {
            PresentationControlRows(snapshot, statusStale);
            ImGui::EndTable();
        }
        PresentationNotes(snapshot);

        // Ray Reconstruction replaces Super Resolution in the game, so their
        // controls share one section.
        const bool rayReconstructionShown = RayReconstructionShown(snapshot);
        const MfgUnlockDlssFeature& dlssView = rayReconstructionShown ? snapshot.dlssRr : snapshot.dlssSr;
        Section(rayReconstructionShown ? T("DLSS Ray Reconstruction") : T("DLSS Super Resolution"));
        if (BeginGrid("##dlsssr"))
        {
            DlssPresetControlRow(dlssView, rayReconstructionShown, statusStale);
            ImGui::EndTable();
        }
        DlssNotes(dlssView, rayReconstructionShown);
        ImGui::EndDisabled();

        // Every readback in one grid, in the order of the controls above.
        Section(T("Status"));
        char value[64]{}, detail[128]{};
        if (BeginGrid("##dlssstatus"))
        {
            GameRows(snapshot, true);
            const bool fgKnown = snapshot.setOptionsSeen && snapshot.setOptionsAccepted;
            snprintf(detail, sizeof(detail), "%s", !snapshot.setOptionsSeen ? T("game intent: unknown")
                : snapshot.gameFrameGenerationOn ? T("game intent: on") : T("game intent: off"));
            StatusDetail(T("FG state"), !fgKnown ? Tone::Dim : snapshot.appliedFrameGenerationOn ? Tone::Good : Tone::Normal,
                !fgKnown ? T("Unknown") : snapshot.appliedFrameGenerationOn ? T("On") : T("Off"), detail);
            if (snapshot.followGameMode)
                StatusRow(T("Requested"), Tone::Normal, "%s", T("Follow game"));
            else if (snapshot.appliedRevision == 0 || snapshot.dynamicMode != snapshot.appliedDynamicMode
                || (!snapshot.dynamicMode && snapshot.desiredMultiplier != snapshot.appliedMultiplier))
                StatusRow(T("Requested"), Tone::Pending, snapshot.dynamicMode ? T("Dynamic") : "%ux",
                    snapshot.desiredMultiplier);
            if (snapshot.appliedRevision != 0)
            {
                snprintf(value, sizeof(value), snapshot.appliedDynamicMode ? T("Dynamic, %ux") : "%ux",
                    snapshot.appliedMultiplier);
                StatusRow(T("Applied"), snapshot.appliedFrameGenerationOn ? Tone::Good : Tone::Normal, "%s", value);
            }
            const char* nvidiaList = !snapshot.nvidiaCompatibilityResolved ? T("NVIDIA list unavailable")
                : snapshot.nvidiaCompatibilityTier == 2 ? T("NVIDIA lists no MFG for this game")
                : snapshot.nvidiaCompatibilityTier != 4 && snapshot.nvidiaCompatibilityTier != 6
                    ? T("game not on NVIDIA's list") : nullptr;
            if (!nvidiaList)
            {
                snprintf(detail, sizeof(detail), T("NVIDIA lists %ux"), snapshot.nvidiaCompatibilityTier);
                nvidiaList = detail;
            }
            if (wrapperControlUnavailable)
                StatusDetail(T("Limit"), Tone::Bad, T("Unavailable"), T("the current wrapper has no verified MFG override"));
            else if (snapshot.activeWrapperObserved)
            {
                snprintf(value, sizeof(value), snapshot.compatibilityFallback ? T("%dx (safe fallback)") : "%dx",
                    safeMaximumMultiplier);
                StatusDetail(T("Limit"), Tone::Normal, value, nvidiaList);
            }
            else
                StatusDetail(T("Limit"), Tone::Pending, T("Waiting"), T("for an active DLSS-G wrapper"));
            char limitHelp[256]{};
            if (snapshot.nvidiaCompatibilityTier == 2)
                snprintf(limitHelp, sizeof(limitHelp), "%s",
                    T("NVIDIA lists ordinary Frame Generation only. The mod keeps its override limit at 2x."));
            else if (snapshot.nvidiaPolicyCeilingMultiplier > static_cast<uint32_t>(safeMaximumMultiplier))
                snprintf(limitHelp, sizeof(limitHelp),
                    T("The listed %ux ceiling requires a compatible runtime. The current limit is %dx."),
                    snapshot.nvidiaPolicyCeilingMultiplier, safeMaximumMultiplier);
            else
                snprintf(limitHelp, sizeof(limitHelp), "%s",
                    T("The highest mode the current DLSS-G runtime supports, and the ceiling\n"
                    "NVIDIA's override list gives this game."));
            HelpMarker(limitHelp);
            if (!gFollowGameMode && gDynamicMode)
            {
                if (gDynamicLockToRefreshRate) snprintf(value, sizeof(value), "%s", T("Display refresh rate"));
                else snprintf(value, sizeof(value), "%d FPS", gDynamicCustomTargetFrameRate);
                if (!snapshot.appliedDynamicTargetValid)
                    StatusDetail(T("Dynamic target"), Tone::Pending, value, T("waiting for an accepted Dynamic request"));
                else
                {
                    if (snapshot.appliedDynamicTargetFrameRate == 0.0f)
                        snprintf(detail, sizeof(detail), "%s", T("applied: display refresh rate"));
                    else
                        snprintf(detail, sizeof(detail), T("applied: %.2f FPS"), snapshot.appliedDynamicTargetFrameRate);
                    StatusDetail(T("Dynamic target"), Tone::Good, value, detail);
                }
            }
            const bool fpsSampleCurrent = snapshot.realFpsMilli > 0
                && snapshot.fpsSampleAgeMs <= 2000;
            if (snapshot.appliedFrameGenerationOn && fpsSampleCurrent)
            {
                if (snapshot.dlssFpsMilli > 0)
                    StatusRow("FPS", Tone::Normal, T("%.1f real, %.1f DLSS"),
                        static_cast<double>(snapshot.realFpsMilli) / 1000.0,
                        static_cast<double>(snapshot.dlssFpsMilli) / 1000.0);
                else
                    StatusRow("FPS", Tone::Normal, T("%.1f real, DLSS unavailable"),
                        static_cast<double>(snapshot.realFpsMilli) / 1000.0);
                PinnedFpsToggle();
            }
            else if (snapshot.appliedFrameGenerationOn && gSampleFrameTelemetry)
            {
                StatusRow("FPS", Tone::Dim, "%s", T("Measuring..."));
                PinnedFpsToggle();
            }
            else if (gPinnedFps.load(std::memory_order_acquire))
            {
                // Keep a pinned readout reachable while frame generation is off.
                StatusRow("FPS", Tone::Dim, "%s", T("DLSS FG off"));
                PinnedFpsToggle();
            }
            PresetStatusRow(presetView);
            PresentationStatusRows(snapshot, statusStale);
            DlssStatusRows(dlssView, rayReconstructionShown);
            ImGui::EndTable();
        }
        DrawWitcherHair();

        Section(T("Advanced"));
        if (ImGui::CollapsingHeader(l10n::Label("Diagnostics")))
        {
            if (!UiAmpere())
            {
                ImGui::BeginDisabled(statusStale);
                if (ImGui::Checkbox("Generated frames only",
                        &gGeneratedOnlyDebug))
                {
                    settingsChanged = true;
                }
                ImGui::EndDisabled();
                HelpMarker("Debug: shows only generated frames.");
            }
            if (ImGui::TreeNode("Presentation"))
            {
                ImGui::Text("Runtime FG V-Sync capability: %s", !snapshot.fgVsyncSupportKnown
                    ? "unknown" : snapshot.fgVsyncSupported ? "supported" : "unavailable");
                DrawPresentationDebug(snapshot);
                ImGui::TreePop();
            }
            if (UiAmpere() && ImGui::TreeNode(snapshot.gpuFamily == 4u ? "Turing RTX" : "Ampere"))
            {
                ImGui::Text("%s | %s | FG preset: %s", snapshot.gpuFamily == 4u ? "Turing SM75" : "Ampere SM86",
                    snapshot.graphicsApi == 2u ? "Vulkan" : snapshot.graphicsApi == 1u ? "D3D12" : "API pending",
                    snapshot.ampereLegacySinglePreset ? "Original preset"
                        : DlssgPresetName(snapshot.dlssgPresetSelectionFrozen
                            ? snapshot.dlssgPresetLatched : snapshot.dlssgPresetRequested));
                ImGui::Text("Ampere provider: %s | Preset query: %s",
                    snapshot.ampereProgramReadyMfg ? "MFG program prepared" : "pending",
                    snapshot.presetQueries ? "observed" : "pending");
                ImGui::Text("Kernel program: %s | Cache status: %u", snapshot.ampereKernelImage == 2 ? "native SM86"
                    : snapshot.ampereKernelImage == 1 ? (snapshot.gpuFamily == 4u ? "SM75 PTX" : "SM86 PTX") : "pending", snapshot.ampereNativeCacheStatus);
                if (snapshot.ampereFailure)
                    ImGui::TextWrapped("Ampere failure: %u (%s)", snapshot.ampereFailure,
                        ampere_diagnostics::FailureName(snapshot.ampereFailure));
                ImGui::Text("Create attempts: %llu | blocked before NGX: %llu | created: %llu",
                    static_cast<unsigned long long>(snapshot.ampereCreateAttempts),
                    static_cast<unsigned long long>(snapshot.ampereCreateBlockedBeforeProvider),
                    static_cast<unsigned long long>(snapshot.ampereCreatedFeatures));
                ImGui::Text("Evaluation attempts: %llu | accepted: %llu",
                    static_cast<unsigned long long>(snapshot.ampereEvaluateAttempts),
                    static_cast<unsigned long long>(snapshot.ampereEvaluations));
                if (snapshot.ampereCandidateVersionMajor)
                    ImGui::Text("Observed provider candidate: %u.%u.%u",
                        snapshot.ampereCandidateVersionMajor, snapshot.ampereCandidateVersionMinor,
                        snapshot.ampereCandidateVersionBuild);
                ImGui::Text("Program preparation: %s (%u)",
                    ampere_diagnostics::PreparationName(snapshot.amperePreparationStage), snapshot.amperePreparationStage);
                if (snapshot.ampereStartupFailureMask)
                    ImGui::TextWrapped("Startup incomplete: %s%s%s%s%s%s%s%s",
                        (snapshot.ampereStartupFailureMask & 0x01u) ? "Streamline startup; " : "",
                        (snapshot.ampereStartupFailureMask & 0x02u) ? "NGX runtime route; " : "",
                        (snapshot.ampereStartupFailureMask & 0x04u) ? "provider route; " : "",
                        (snapshot.ampereStartupFailureMask & 0x08u) ? "startup capacity; " : "",
                        (snapshot.ampereStartupFailureMask & 0x10u) ? "D3D12/Vulkan device; " : "",
                        (snapshot.ampereStartupFailureMask & 0x20u) ? "Ampere adapter; " : "",
                        (snapshot.ampereStartupFailureMask & 0x40u) ? "kernel program; " : "",
                        (snapshot.ampereStartupFailureMask & 0x80u) ? "fatal boundary; " : "");
                if (snapshot.amperePrimaryFailure)
                    ImGui::TextWrapped("Current first failure: %u (%s) | NGX: 0x%08X",
                        snapshot.amperePrimaryFailure, ampere_diagnostics::FailureName(snapshot.amperePrimaryFailure),
                        snapshot.amperePrimaryNgxResult);
                if (snapshot.ampereFirstFailure)
                    ImGui::TextWrapped("First failure this run: %u (%s) | NGX: 0x%08X",
                        snapshot.ampereFirstFailure, ampere_diagnostics::FailureName(snapshot.ampereFirstFailure),
                        snapshot.ampereFirstNgxResult);
                if (snapshot.ampereLastFailure && snapshot.ampereLastFailure != snapshot.amperePrimaryFailure)
                    ImGui::TextWrapped("Latest failure: %u (%s) | NGX: 0x%08X",
                        snapshot.ampereLastFailure, ampere_diagnostics::FailureName(snapshot.ampereLastFailure),
                        snapshot.ampereLastNgxResult);
                ImGui::TreePop();
            }
            if (ImGui::TreeNode("Active route"))
            {
                ImGui::Text("Core protocol: %u | Bridge: %s",
                    snapshot.statusProtocolVersion,
                    snapshot.bridgeReady ? "ready" : "not ready");
                ImGui::Text("Fail-closed reason: %s (%u)",
                    snapshot.universalRouteFailureReason[0]
                        ? snapshot.universalRouteFailureReason : "none",
                    snapshot.universalRouteFailure);
                ImGui::TextWrapped("Wrapper: %s",
                    snapshot.activeWrapperPath[0]
                        ? snapshot.activeWrapperPath : "waiting for a real call");
                ImGui::Text("Wrapper version: %u.%u.%u.%u | generation: %llu",
                    snapshot.activeWrapperVersionMajor,
                    snapshot.activeWrapperVersionMinor,
                    snapshot.activeWrapperVersionBuild,
                    snapshot.activeWrapperVersionPrivate,
                    static_cast<unsigned long long>(
                        snapshot.activeWrapperGeneration));
                ImGui::Text("Control: %s (%s) | State: %s (%s)",
                    snapshot.activeControlPath[0]
                        ? snapshot.activeControlPath : "none",
                    snapshot.activeControlDetour[0]
                        ? snapshot.activeControlDetour : "none",
                    snapshot.activeStatePath[0]
                        ? snapshot.activeStatePath : "none",
                    snapshot.activeStateDetour[0]
                        ? snapshot.activeStateDetour : "none");
                ImGui::TreePop();
            }
            if (ImGui::TreeNode("Active provider"))
            {
                ImGui::TextWrapped("Provider: %s",
                    snapshot.activeProviderPath[0]
                        ? snapshot.activeProviderPath : "waiting for FG Create");
                ImGui::Text("Provider version: %u.%u.%u.%u | generation: %llu",
                    snapshot.activeProviderVersionMajor,
                    snapshot.activeProviderVersionMinor,
                    snapshot.activeProviderVersionBuild,
                    snapshot.activeProviderVersionPrivate,
                    static_cast<unsigned long long>(
                        snapshot.activeProviderGeneration));
                ImGui::Text("Selected by: %s | Create: %s | Evaluate: %s",
                    snapshot.providerSelectionSource[0]
                        ? snapshot.providerSelectionSource : "none",
                    snapshot.providerCreateDetour[0]
                        ? snapshot.providerCreateDetour : "none",
                    snapshot.providerEvaluateDetour[0]
                        ? snapshot.providerEvaluateDetour : "none");
                ImGui::Text("Midpoint ready at first Create: %s",
                    YesNo(snapshot.midpointReadyAtFirstCreate));
                ImGui::TreePop();
            }
            if (ImGui::TreeNode("Control and lifecycle"))
            {
                ImGui::Text("Requested/applied revision: %llu/%llu",
                    static_cast<unsigned long long>(snapshot.desiredRevision),
                    static_cast<unsigned long long>(snapshot.appliedRevision));
                ImGui::Text("Route last call/accepted revision: %llu/%llu",
                    static_cast<unsigned long long>(
                        snapshot.activeLastCallRevision),
                    static_cast<unsigned long long>(
                        snapshot.activeLastAcceptedRevision));
                ImGui::Text("FG applied/intent: %s/%s | Off accepted: %s | Release observed: %s",
                    snapshot.appliedFrameGenerationOn ? "on" : "off",
                    snapshot.gameFrameGenerationOn ? "on" : "off",
                    YesNo(snapshot.frameGenerationOffAccepted),
                    YesNo(snapshot.releaseObserved));
                ImGui::Text("Release entry: %s | Recreate required: %s",
                    snapshot.releaseEntryCurrent ? "covered" : "unavailable",
                    YesNo(snapshot.streamlineRebuildRequired));
                ImGui::Text("NVIDIA max: %s | Available max: %s",
                    snapshot.nvidiaCompatibilityTier == 4
                            || snapshot.nvidiaCompatibilityTier == 6
                        ? (snapshot.nvidiaCompatibilityTier == 6 ? "6x" : "4x")
                        : snapshot.nvidiaCompatibilityTier == 2 ? "no MFG override listed" : "unknown",
                    snapshot.activeWrapperObserved
                        ? (safeMaximumMultiplier == 6 ? "6x"
                            : safeMaximumMultiplier == 5 ? "5x"
                            : safeMaximumMultiplier == 4 ? "4x"
                            : safeMaximumMultiplier == 3 ? "3x" : "2x")
                        : "detecting");
                if (snapshot.getStateSeen && snapshot.lastGetStateResult == 0)
                {
                    ImGui::Text("Runtime: %u presented | max %u generated",
                        snapshot.actualFramesPresented,
                        snapshot.numFramesToGenerateMax);
                }
                ImGui::Text("DLSS FPS source: %s", snapshot.fpsOutputSource == 4 ? "DLSS-G presented-frame count"
                    : snapshot.fpsOutputSource == 2 ? "DXGI present count"
                    : snapshot.fpsOutputSource == 1 ? "present callback" : "unavailable");
                ImGui::TreePop();
            }
            if (ImGui::TreeNode("Temporal interval trace"))
            {
                ImGui::Text("Log: %s | Samples: %llu valid, %llu invalid, %llu dropped",
                    snapshot.intervalLogReady ? "ready" : "opening",
                    static_cast<unsigned long long>(snapshot.intervalValidSamples),
                    static_cast<unsigned long long>(snapshot.intervalInvalidSamples),
                    static_cast<unsigned long long>(snapshot.intervalDroppedSamples));
                if (snapshot.intervalValidSamples > 0)
                {
                    ImGui::Text("Last interval: count %d | index %d | position %u/%u",
                        snapshot.intervalLastCount, snapshot.intervalLastIndex,
                        snapshot.intervalLastPositionNumerator,
                        snapshot.intervalLastPositionDenominator);
                }
                if (snapshot.intervalLogFile[0])
                    ImGui::Text("Trace: %s, beside the runtime log", snapshot.intervalLogFile);
                ImGui::TreePop();
            }
            ImGui::Text("UI recomposition: %s", snapshot.gameUiRecompositionEnabled ? "on (the game's own)" : "off");
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("DLSS-G UI recomposition is left to the game; RTXMFG never enables it.\n"
                    "An NVIDIA frame generation preset can still turn it on in the driver.");
        }
        DrawWitcherHairDiagnostics();
        if (tabs) ImGui::EndTabItem();
    }
    if (tabs)
    {
        if (TabItem(T("DLSS 5")))
        {
            DrawDlss5(snapshot, statusStale);
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }

    if (settingsChanged)
        ApplySettings(runtime, true);
}
}

namespace standalone_ui
{
void Initialize(HMODULE self)
{
    gSelf = self;
    if (!gInitialized.exchange(true, std::memory_order_acq_rel))
    {
        LoadSettings();
        const auto path = SettingsPath("RTX40MFG-UI.ini");
        gPinnedFps.store(!path.empty() && GetPrivateProfileIntW(L"Overlay", L"PinnedFps", 0, path.c_str()) == 1,
            std::memory_order_release);
    }
}
bool PinnedFps()
{
    return gPinnedFps.load(std::memory_order_acquire);
}
void Draw()
{
    gLabelWidth = gLabelWidthSeen;
    gLabelWidthSeen = 0.0f;
    DrawSettings(nullptr);
}
void DrawFooterStatus()
{
    if (!gFooterStatus[0]) return;
    const char* text = T(gFooterStatus);
    const float width = ImGui::CalcTextSize(text).x;
    ImGui::SameLine();
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(0.0f, ImGui::GetContentRegionAvail().x - width));
    ImGui::AlignTextToFramePadding();
    ImGui::TextColored(ToneColor(gFooterTone), "%s", text);
    if (std::strcmp(gFooterStatus, "Paused") == 0 && ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", T("Status is temporarily unavailable; controls are paused."));
}
void DrawPinnedFps()
{
    // The same values as the menu's FPS row. The backend rewrites its status
    // about once a second; read it at most twice a second during play.
    static char text[96]{};
    static bool live = false;
    static ULONGLONG nextRead = 0;
    const ULONGLONG now = GetTickCount64();
    if (now >= nextRead)
    {
        nextRead = now + 500;
        MfgUnlockReShadeSnapshot snapshot{};
        if (!BackendGetSnapshot(&snapshot))
        {
            snprintf(text, sizeof(text), "%s", T("FPS status unavailable"));
            live = false;
        }
        // xemfgsa.5: XeMFG standalone's frame rates (the snapshot read above refreshed gXessUi from the same status).
        else if (xess_ui::XemfgRuns(gXessUi))
        {
            const auto& x = gXessUi;
            live = x.xemfgGenerating && xess_ui::XemfgFpsCurrent(x);
            if (!x.xemfgGenerating)
                snprintf(text, sizeof(text), "%s", T("XeMFG off"));
            else if (!live)
                snprintf(text, sizeof(text), "%s", T("Real -- | XeMFG --"));
            else
                snprintf(text, sizeof(text), T("Real %.0f | XeMFG %.0f"), x.xemfgBaseFpsMilli / 1000.0,
                    x.xemfgOutputFpsMilli / 1000.0);
        }
        else if (!snapshot.appliedFrameGenerationOn)
        {
            snprintf(text, sizeof(text), "%s", T("DLSS FG off"));
            live = false;
        }
        else if (snapshot.realFpsMilli == 0 || snapshot.fpsSampleAgeMs > 2000)
        {
            snprintf(text, sizeof(text), "%s", T("Real -- | DLSS --"));
            live = false;
        }
        else
        {
            const double real = static_cast<double>(snapshot.realFpsMilli) / 1000.0;
            if (snapshot.dlssFpsMilli > 0)
                snprintf(text, sizeof(text), T("Real %.0f | DLSS %.0f"), real,
                    static_cast<double>(snapshot.dlssFpsMilli) / 1000.0);
            else
                snprintf(text, sizeof(text), T("Real %.0f | DLSS --"), real);
            live = true;
        }
    }
    // The menu's status green while values are live; its dim text otherwise.
    ImGui::TextColored(ToneColor(live ? Tone::Good : Tone::Dim), "%s", text);
}
}
