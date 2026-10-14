#include "build_variant.h"
#include "unified_control_paths.h"
#include "status_transport.h"
#include "ampere_policy.h"
#include "nvidia_mfg_policy.h"
#include "ui_dynamic_mfg.h"
#include "universal_route_policy.h"
#include "reshade_frontend.h"

#if defined(MFG_UNLOCK_RESHADE_UI_CLIENT)
#include "backend_bridge.h"
#else
#include "reshade_bridge.h"
#endif
#include "ampere_diagnostics.h"
#include "reflex_control.h"
#include "vsync_control.h"

#include <imgui.h>
#if defined(MFG_UNLOCK_SINGLE_MODULE_UI)
#include "ui_host.h"
#include "standalone_ui.h"
#include "single_module.h"
#include "hudless_visualizer.h"
#include "witcher_dots/witcher_dots.h"
#else
#include <reshade.hpp>
#endif

#include <Psapi.h>
#include <d3d12.h>
#include <dxgi.h>

#include <algorithm>
#include <charconv>
#include <cmath>
#include "ui_status_json.h"
#include "xess_ui_state.h"
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
constexpr const char* kConfigSection = MFG_PRODUCT;
constexpr const char* kOverlayTitle = MFG_OVERLAY_TITLE;
constexpr const char* kReShadeHomeWindow = "###home";

std::atomic<uint32_t> gGpuFamily{0};
bool UiAmpere() noexcept { return gGpuFamily.load(std::memory_order_acquire) == 2u; }

std::atomic<bool> gRegistered{false};
std::atomic<bool> gInitialized{false};
HMODULE gSelf = nullptr;
HMODULE gReShade = nullptr;
bool gDefaultDockInitialized = false;
bool gOverlayVisitedThisFrame = false;
bool gOverlayDockedThisFrame = false;
#if defined(MFG_UNLOCK_RESHADE_UI_CLIENT)
enum class BackendKind : uint32_t
{
    eNone = 0,
    eAbi = 1,
    eNativeFile = 2,
};

HMODULE gBackend = nullptr;
MfgUnlockBackendInterface gBackendInterface{};
std::atomic<bool> gBackendConnected{false};
std::atomic<BackendKind> gBackendKind{BackendKind::eNone};
std::atomic<bool> gControlCompanionBackend{false};
std::atomic<bool> gIntegratedUniversalBackend{false};
SRWLOCK gBackendConnectionLock = SRWLOCK_INIT;
std::wstring gNativeConfigPath;
std::wstring gNativeStatusPath;
// Display-only state. Control writes always obtain their own current snapshot.
MfgUnlockReShadeSnapshot gLastNativeDisplay{};
std::wstring gLastNativeDisplayPath;
bool gHaveNativeDisplay = false;
std::wstring gCompanionConfigPath;
using MfgUnlockSampleFrameTelemetryFn = BOOL (WINAPI*)();
MfgUnlockSampleFrameTelemetryFn gSampleFrameTelemetry = nullptr;
std::atomic<uintptr_t> gTelemetrySwapchain{0};
std::atomic<uint64_t> gTelemetrySwapchainTick{0};

struct NativeMidpointUiState
{
    bool available = false;
    bool required = false;
    bool ready = false;
    bool publicRouteReady = false;
    BOOL adapterObserved = FALSE;
    BOOL backportEnabled = FALSE;
    BOOL backportApplied = FALSE;
    BOOL readyAtCreate = FALSE;
    BOOL fallbackActive = TRUE;
    uint32_t rawCount = 0;
    uint32_t uniqueOutputs = 0;
};

NativeMidpointUiState gNativeMidpoint{};

bool ConnectBackend() noexcept;
void MergeNativeMidpointStatus(
    MfgUnlockReShadeSnapshot& snapshot) noexcept;
#endif
SRWLOCK gPresentationQueueLock = SRWLOCK_INIT;
ID3D12CommandQueue* gPresentationQueue = nullptr;

int gMultiplier = 2;
bool gFollowGameMode = true;
bool gDynamicMode = false;
int gDynamicTargetFrameRate = 0;
bool gDynamicLockToRefreshRate = true;
int gDynamicCustomTargetFrameRate = 60;
int gDlssgPreset = 2;
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

#if !defined(MFG_UNLOCK_RESHADE_UI_CLIENT)
extern "C" BOOL WINAPI MfgUnlockRegisterD3D12Device(
    ID3D12Device* device);
extern "C" BOOL WINAPI MfgUnlockRegisterD3D12Queue(
    ID3D12CommandQueue* queue);
extern "C" BOOL WINAPI MfgUnlockRegisterD3D12Swapchain(
    IDXGISwapChain* swapchain, IUnknown* presentationQueue);
extern "C" BOOL WINAPI MfgUnlockUnregisterD3D12Swapchain(
    IDXGISwapChain* swapchain);
#endif

BOOL BackendGetSnapshot(MfgUnlockReShadeSnapshot* snapshot) noexcept
{
#if defined(MFG_UNLOCK_RESHADE_UI_CLIENT)
    if (!ConnectBackend() || !gBackendInterface.getSnapshot
        || !gBackendInterface.getSnapshot(snapshot))
    {
        return FALSE;
    }
    if (gControlCompanionBackend.load(std::memory_order_acquire))
        MergeNativeMidpointStatus(*snapshot);
    return TRUE;
#else
    return MfgUnlockReShadeGetSnapshot(snapshot);
#endif
}

BOOL BackendApplyControl(uint32_t multiplier, BOOL dynamicMode,
    uint32_t dynamicTargetFrameRate, BOOL dynamicExperimental56,
    BOOL generatedOnlyDebug) noexcept
{
#if defined(MFG_UNLOCK_RESHADE_UI_CLIENT)
    return ConnectBackend() && gBackendInterface.applyControl
        ? gBackendInterface.applyControl(multiplier, dynamicMode,
            dynamicTargetFrameRate, dynamicExperimental56,
            generatedOnlyDebug) : FALSE;
#else
    return MfgUnlockReShadeApplyControl(multiplier, dynamicMode,
        dynamicTargetFrameRate, dynamicExperimental56,
        generatedOnlyDebug);
#endif
}

BOOL BackendRegisterDevice(ID3D12Device* device) noexcept
{
#if defined(MFG_UNLOCK_RESHADE_UI_CLIENT)
    return ConnectBackend() && gBackendInterface.registerD3D12Device
        ? gBackendInterface.registerD3D12Device(device) : FALSE;
#else
    return MfgUnlockRegisterD3D12Device(device);
#endif
}

BOOL BackendRegisterQueue(ID3D12CommandQueue* queue) noexcept
{
#if defined(MFG_UNLOCK_RESHADE_UI_CLIENT)
    return ConnectBackend() && gBackendInterface.registerD3D12Queue
        ? gBackendInterface.registerD3D12Queue(queue) : FALSE;
#else
    return MfgUnlockRegisterD3D12Queue(queue);
#endif
}

BOOL BackendRegisterSwapchain(IDXGISwapChain* swapchain,
    IUnknown* presentationQueue) noexcept
{
#if defined(MFG_UNLOCK_RESHADE_UI_CLIENT)
    return ConnectBackend() && gBackendInterface.registerD3D12Swapchain
        ? gBackendInterface.registerD3D12Swapchain(
            swapchain, presentationQueue) : FALSE;
#else
    return MfgUnlockRegisterD3D12Swapchain(
        swapchain, presentationQueue);
#endif
}

BOOL BackendUnregisterSwapchain(IDXGISwapChain* swapchain) noexcept
{
#if defined(MFG_UNLOCK_RESHADE_UI_CLIENT)
    return ConnectBackend() && gBackendInterface.unregisterD3D12Swapchain
        ? gBackendInterface.unregisterD3D12Swapchain(swapchain) : FALSE;
#else
    return MfgUnlockUnregisterD3D12Swapchain(swapchain);
#endif
}

#if defined(MFG_UNLOCK_RESHADE_UI_CLIENT)
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
#if MFG_UNLOCK_RUNTIME_GPU_SELECTION
    gNativeConfigPath = unified_control_paths::Config(directory.wstring());
    gNativeStatusPath = unified_control_paths::Status(gNativeConfigPath, directory.wstring());
#elif defined(MFG_UNLOCK_V12_UNIVERSAL_UI)
    gNativeConfigPath = (directory
        / MFG_CONFIG_W).wstring();
    gNativeStatusPath = (directory
        / MFG_STATUS_W).wstring();
#else
    if (GetModuleHandleW(L"RTX40MFG-Universal.asi"))
    {
        gNativeConfigPath = (directory
            / MFG_CONFIG_W).wstring();
        gNativeStatusPath = (directory
            / MFG_STATUS_W).wstring();
    }
    else
    {
        gNativeConfigPath = (directory / L"plugins"
            / L"cyber_engine_tweaks" / L"mods" / MFG_PRODUCT_W
            / L"config.json").wstring();
        gNativeStatusPath = (directory / L"plugins"
            / L"cyber_engine_tweaks" / L"mods" / MFG_PRODUCT_W
            / L"bridge_status.json").wstring();
    }
#endif
    return true;
}

bool ResolveCompanionControlPath() noexcept
{
    std::wstring executablePath(32768, L'\0');
    const DWORD length = GetModuleFileNameW(nullptr, executablePath.data(),
        static_cast<DWORD>(executablePath.size()));
    if (length == 0 || length >= executablePath.size())
        return false;
    executablePath.resize(length);
    gCompanionConfigPath =
        (std::filesystem::path(executablePath).parent_path()
            / L"RTX40MFG-Bridge.json").wstring();
    return true;
}

BOOL PersistCompanionControl() noexcept
{
    if (!gControlCompanionBackend.load(std::memory_order_acquire))
        return TRUE;
    if (gCompanionConfigPath.empty() && !ResolveCompanionControlPath())
        return FALSE;

    char content[320]{};
    const int length = std::snprintf(content, sizeof(content),
        "{\"version\":1,\"followGame\":%s,\"mode\":\"%s\","
        "\"multiplier\":%u,\"dynamicTargetFrameRate\":%u,"
        "\"dynamicExperimental56\":%s,\"generatedOnlyDebug\":%s}\n",
        gFollowGameMode ? "true" : "false",
        gDynamicMode ? "dynamic" : "fixed",
        static_cast<uint32_t>(std::clamp(gMultiplier, UiAmpere() ? 1 : 2, 6)),
        static_cast<uint32_t>(
            std::clamp(gDynamicTargetFrameRate, 0, 1000)),
        gDynamicExperimental56 ? "true" : "false",
        gGeneratedOnlyDebug ? "true" : "false");
    if (length <= 0 || static_cast<size_t>(length) >= sizeof(content))
        return FALSE;

    const std::wstring temporary = gCompanionConfigPath + L".tmp";
    HANDLE file = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr,
        CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return FALSE;
    DWORD written = 0;
    const BOOL write = WriteFile(file, content, static_cast<DWORD>(length),
        &written, nullptr);
    const BOOL flushed = write && written == static_cast<DWORD>(length)
        && FlushFileBuffers(file);
    CloseHandle(file);
    if (!flushed || !MoveFileExW(temporary.c_str(),
            gCompanionConfigPath.c_str(),
            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
    {
        DeleteFileW(temporary.c_str());
        return FALSE;
    }
    return TRUE;
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

#if defined(MFG_UNLOCK_SINGLE_MODULE_UI)
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
#endif

BOOL WINAPI NativeFileGetSnapshot(MfgUnlockReShadeSnapshot* snapshot)
{
    if (!snapshot || snapshot->structSize < sizeof(*snapshot)) return FALSE;
    MfgUnlockReShadeSnapshot output{};
    const bool haveSavedControl = ReadNativeControlSnapshot(&output) != FALSE;
    std::string status;
    const auto statusSource=status_transport::Read(gNativeStatusPath,status,&ReadTextFile,&ValidNativeStatus);
    if(statusSource==status_transport::Source::None)return FALSE;
#if defined(MFG_UNLOCK_SINGLE_MODULE_UI)
    static auto previousSource=status_transport::Source::None;
    if(statusSource!=previousSource){
        single_module::Log(statusSource==status_transport::Source::Fallback
            ?L"MFG_STATUS_UI fresh snapshot source=process-memory"
            :L"MFG_STATUS_UI fresh snapshot source=config-directory");
        previousSource=statusSource;
    }
#endif
    {
        // A status heartbeat can precede the latest atomic config save. Saved
        // intent remains authoritative; applied state comes only from status.
        if (!haveSavedControl) ParseJsonBool(status, "followGame", output.followGameMode);
        ParseJsonInteger(status, "version",
            output.statusProtocolVersion);
        if (!ParseJsonInteger(status, "gpuFamily", output.gpuFamily) || output.gpuFamily > 3u)
            return FALSE;
        ParseJsonInteger(status, "gpuAdapterLuid", output.gpuAdapterLuid);
        ParseJsonInteger(status, "gpuSelectionFailure", output.gpuSelectionFailure);
        gGpuFamily.store(output.gpuFamily, std::memory_order_release);
#if defined(MFG_UNLOCK_SINGLE_MODULE_UI)
        ParseXessUiState(status);
#endif
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
        ParseJsonBool(status, "selectiveOtaDlssgWrapperRequested",
            output.selectiveOtaDlssgWrapperRequested);
        ParseJsonBool(status, "selectiveOtaDlssgWrapperCandidateReady",
            output.selectiveOtaDlssgWrapperCandidateReady);
        ParseJsonInteger(status, "selectiveOtaDlssgWrapperFailure",
            output.selectiveOtaDlssgWrapperFailure);
        ParseJsonInteger(status,
            "selectiveOtaDlssgWrapperRedirectAttempts",
            output.selectiveOtaDlssgWrapperRedirectAttempts);
        ParseJsonInteger(status,
            "selectiveOtaDlssgWrapperRedirectSuccesses",
            output.selectiveOtaDlssgWrapperRedirectSuccesses);
        ParseJsonInteger(status, "selectiveOtaDlssgWrapperFallbacks",
            output.selectiveOtaDlssgWrapperFallbacks);
        ParseJsonInteger(status, "selectiveOtaDlssgWrapperVersionMajor",
            output.selectiveOtaDlssgWrapperVersionMajor);
        ParseJsonInteger(status, "selectiveOtaDlssgWrapperVersionMinor",
            output.selectiveOtaDlssgWrapperVersionMinor);
        ParseJsonInteger(status, "selectiveOtaDlssgWrapperVersionBuild",
            output.selectiveOtaDlssgWrapperVersionBuild);
        ParseJsonInteger(status, "selectiveOtaDlssgWrapperVersionPrivate",
            output.selectiveOtaDlssgWrapperVersionPrivate);
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
        ParseJsonInteger(status, "fpsSampleAgeMs", output.fpsSampleAgeMs);
        ParseJsonBool(status, "uiInputsReady", output.uiInputsReady);
        ParseJsonBool(status, "uiRecompositionEnabled",
            output.uiRecompositionEnabled);
        ParseJsonBool(status, "uiRecompositionForced",
            output.uiRecompositionForced);
        ParseJsonBool(status, "hudlessDetectEnabled", output.hudlessDetectEnabled);
        const auto copyString = [&status](const char* key, char* target, size_t size) {
            std::string value;
            if (ParseJsonString(status, key, value)) strncpy_s(target, size, value.c_str(), _TRUNCATE);
        };
        copyString("hudlessDetectSource", output.hudlessDetectSource, sizeof(output.hudlessDetectSource));
        copyString("hudlessDetectVerdict", output.hudlessDetectVerdict, sizeof(output.hudlessDetectVerdict));
        copyString("hudlessDetectRoute", output.hudlessDetectRoute, sizeof(output.hudlessDetectRoute));
        copyString("hudlessDetectReason", output.hudlessDetectReason, sizeof(output.hudlessDetectReason));
        copyString("hudlessDetectBestHypothesis", output.hudlessDetectBestHypothesis,
            sizeof(output.hudlessDetectBestHypothesis));
        ParseJsonInteger(status, "hudlessDetectProbes", output.hudlessDetectProbes);
        ParseJsonInteger(status, "hudlessDetectConclusive", output.hudlessDetectConclusive);
        ParseJsonInteger(status, "hudlessDetectIdentityPermille", output.hudlessDetectIdentityPermille);
        ParseJsonInteger(status, "hudlessDetectBestPermille", output.hudlessDetectBestPermille);
        ParseJsonInteger(status, "hudlessDetectUiCoveragePermille", output.hudlessDetectUiCoveragePermille);
        ParseJsonInteger(status, "hudlessDetectFormat", output.hudlessDetectFormat);
        ParseJsonInteger(status, "hudlessDetectExtentWidth", output.hudlessDetectExtentWidth);
        ParseJsonInteger(status, "hudlessDetectExtentHeight", output.hudlessDetectExtentHeight);
        ParseJsonInteger(status, "hudlessDetectLifecycle", output.hudlessDetectLifecycle);
        ParseJsonInteger(status, "hudlessDetectFinalFormat", output.hudlessDetectFinalFormat);
        ParseJsonInteger(status, "hudlessDetectFinalWidth", output.hudlessDetectFinalWidth);
        ParseJsonInteger(status, "hudlessDetectFinalHeight", output.hudlessDetectFinalHeight);
        ParseJsonInteger(status, "hudlessDetectCompositePermille", output.hudlessDetectCompositePermille);
        ParseJsonInteger(status, "hudlessDetectTranslucentTiles", output.hudlessDetectTranslucentTiles);
        ParseJsonInteger(status, "hudlessDetectInformativeTiles", output.hudlessDetectInformativeTiles);
        copyString("hudRecompositionState", output.hudRecompositionState, sizeof(output.hudRecompositionState));
        ParseJsonBool(status, "hudRecompositionVerified", output.hudRecompositionVerified);
        ParseJsonBool(status, "hudlessDetectPaired", output.hudlessDetectPaired);
        ParseJsonInteger(status, "hudlessDetectStillPermille", output.hudlessDetectStillPermille);
        ParseJsonInteger(status, "hudlessPresentMarkers", output.hudlessPresentMarkers);
        ParseJsonInteger(status, "hudlessTagLead", output.hudlessTagLead);
        ParseJsonInteger(status, "hudlessMaxTagLead", output.hudlessMaxTagLead);
        copyString("hudRecompositionUi", output.hudRecompositionUi, sizeof(output.hudRecompositionUi));
        ParseJsonBool(status, "hudlessDetectDiffersInMotion", output.hudlessDetectDiffersInMotion);
        ParseJsonInteger(status, "hudlessDetectMotionDiffersProbes", output.hudlessDetectMotionDiffersProbes);
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

void MergeNativeMidpointStatus(
    MfgUnlockReShadeSnapshot& snapshot) noexcept
{
    NativeMidpointUiState state{};
    state.publicRouteReady = snapshot.bridgeReady != FALSE;
    state.required = snapshot.dynamicMode != FALSE
        || snapshot.desiredMultiplier > 2
        || snapshot.appliedMultiplier > 2;

    std::string status;
    BOOL rebuildRequired = FALSE;
    if (ReadTextFile(gNativeStatusPath, status))
    {
        const bool complete =
            ParseJsonBool(status, "adaAuthoritativeD3D12DeviceObserved",
                state.adapterObserved)
            && ParseJsonBool(status, "adaTemporalBackportEnabled",
                state.backportEnabled)
            && ParseJsonBool(status, "adaTemporalBackportApplied",
                state.backportApplied)
            && ParseJsonBool(status,
                "ngxFrameGenerationBackportReadyAtCreate",
                state.readyAtCreate)
            && ParseJsonBool(status, "synthesisFallbackActive",
                state.fallbackActive);
        state.available = complete;
        ParseJsonBool(status, "streamlineRebuildRequired",
            rebuildRequired);
        ParseJsonInteger(status, "ngxRawCount", state.rawCount);
        ParseJsonInteger(status, "ngxLastOutputUniqueCount",
            state.uniqueOutputs);
    }
    state.ready = state.available && state.adapterObserved
        && state.backportEnabled && state.backportApplied
        && state.readyAtCreate && !state.fallbackActive;
    gNativeMidpoint = state;
    // Midpoint/backport telemetry belongs to the optional research backend.
    // It must not override the public route reported by the control companion
    // or prevent the proven native ASI from serving ordinary MFG requests.
    (void)rebuildRequired;
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
        for (const char* name : {"xessModel", "optiScalerUi"})
            CarryControlField(content, saved, name);
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
    if (current.gpuFamily != 1u && current.gpuFamily != 2u) return FALSE;
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

BOOL NativeFilePersistPreset(uint32_t preset)
{
    if (preset > 3) return FALSE;
    std::string content;
    if (ReadTextFile(gNativeConfigPath, content))
    {
        MfgUnlockReShadeSnapshot saved{};
        if (!ParseNativeControlSnapshot(content, &saved)
            || !SetUnsignedControlField(content, "dlssgPreset", preset)
            || !SetUnsignedControlField(content, "version", 13)) return FALSE;
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
            current.intervalLoggingEnabled, preset, current.vsyncMode, current.reflexFrameLimitFps);
    }
    // Preset changes do not need Dynamic capability or an active FG feature.
    // Preserve the saved request and let the backend's immutable preset state
    // decide whether the choice belongs to this process or the next restart.
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
#if defined(MFG_UNLOCK_V12_UNIVERSAL_UI)
#if defined(MFG_UNLOCK_SINGLE_MODULE_UI)
    HMODULE core = gSelf;
    HMODULE shim = gSelf;
#else
    HMODULE core = GetModuleHandleW(MFG_CORE_W);
    HMODULE shim = GetModuleHandleW(MFG_SHIM_W);
#endif
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
    gControlCompanionBackend.store(false, std::memory_order_release);
    gIntegratedUniversalBackend.store(true, std::memory_order_release);
    gBackendConnected.store(true, std::memory_order_release);
    ReleaseSRWLockExclusive(&gBackendConnectionLock);
    return true;
#endif
    HMODULE modules[1024]{};
    DWORD bytes = 0;
    if (!K32EnumProcessModules(GetCurrentProcess(), modules,
            sizeof(modules), &bytes))
    {
        ReleaseSRWLockExclusive(&gBackendConnectionLock);
        return false;
    }

    HMODULE backend = nullptr;
    HMODULE nativeBackend = nullptr;
    MfgUnlockBackendInterface interfaceTable{};
    ResolveNativeFilePaths();
    const DWORD count = std::min<DWORD>(bytes / sizeof(HMODULE),
        static_cast<DWORD>(std::size(modules)));
    for (DWORD index = 0; index < count; ++index)
    {
        HMODULE const candidate = modules[index];
        auto* const query = candidate
            ? reinterpret_cast<MfgUnlockBackendQueryInterfaceFn>(
                GetProcAddress(candidate,
                    "MfgUnlockBackendQueryInterface"))
            : nullptr;
        MfgUnlockBackendInterface candidateInterface{};
        if (!query
            || !query(MFG_UNLOCK_BACKEND_ABI_V3, &candidateInterface,
                sizeof(candidateInterface))
            || candidateInterface.structSize < sizeof(candidateInterface)
            || candidateInterface.abiVersion != MFG_UNLOCK_BACKEND_ABI_V3
            || !candidateInterface.getSnapshot
            || !candidateInterface.applyControl
            || !candidateInterface.registerD3D12Device
            || !candidateInterface.registerD3D12Queue
            || !candidateInterface.registerD3D12Swapchain
            || !candidateInterface.unregisterD3D12Swapchain
            || !candidateInterface.setFrontendAttached)
        {
            continue;
        }

        HMODULE pinned = nullptr;
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS
                | GET_MODULE_HANDLE_EX_FLAG_PIN,
                reinterpret_cast<LPCWSTR>(query), &pinned)
            || pinned != candidate)
        {
            continue;
        }
        backend = candidate;
        interfaceTable = candidateInterface;
        std::wstring backendPath(32768, L'\0');
        const DWORD backendPathLength = GetModuleFileNameW(candidate,
            backendPath.data(), static_cast<DWORD>(backendPath.size()));
        if (backendPathLength > 0
            && backendPathLength < backendPath.size())
        {
            backendPath.resize(backendPathLength);
        }
        else
        {
            backendPath.clear();
        }
        const bool controlCompanion = !backendPath.empty()
            && _wcsicmp(std::filesystem::path(backendPath).filename().c_str(),
                L"RTX40MFG-Bridge.asi") == 0;
        const bool integratedUniversal = !backendPath.empty()
            && _wcsicmp(std::filesystem::path(backendPath).filename().c_str(),
                L"RTX40MFG-Universal.asi") == 0;
        gControlCompanionBackend.store(
            controlCompanion, std::memory_order_release);
        gIntegratedUniversalBackend.store(
            integratedUniversal, std::memory_order_release);
        ResolveNativeFilePaths();
        break;

        // Unreachable after a valid ABI candidate; native discovery is done
        // below for candidates which do not expose the versioned interface.
    }
    if (!backend)
    {
        for (DWORD index = 0; index < count; ++index)
        {
            HMODULE const candidate = modules[index];
            std::wstring path(32768, L'\0');
            const DWORD pathLength = candidate
                ? GetModuleFileNameW(candidate, path.data(),
                    static_cast<DWORD>(path.size()))
                : 0;
            if (pathLength == 0 || pathLength >= path.size())
            {
                continue;
            }
            path.resize(pathLength);
            if (_wcsicmp(std::filesystem::path(path).filename().c_str(),
                    MFG_SHIM_W) != 0)
            {
                continue;
            }
            auto registerDevice = reinterpret_cast<
                MfgUnlockRegisterD3D12DeviceFn>(GetProcAddress(candidate,
                    "MfgUnlockRegisterD3D12Device"));
            auto registerQueue = reinterpret_cast<
                MfgUnlockRegisterD3D12QueueFn>(GetProcAddress(candidate,
                    "MfgUnlockRegisterD3D12Queue"));
            auto registerSwapchain = reinterpret_cast<
                MfgUnlockRegisterD3D12SwapchainFn>(GetProcAddress(candidate,
                    "MfgUnlockRegisterD3D12Swapchain"));
            auto unregisterSwapchain = reinterpret_cast<
                MfgUnlockUnregisterD3D12SwapchainFn>(GetProcAddress(candidate,
                    "MfgUnlockUnregisterD3D12Swapchain"));
            HMODULE pinned = nullptr;
            if (!registerDevice || !registerQueue || !registerSwapchain
                || !unregisterSwapchain || !ResolveNativeFilePaths()
                || !GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS
                        | GET_MODULE_HANDLE_EX_FLAG_PIN,
                    reinterpret_cast<LPCWSTR>(registerQueue), &pinned)
                || pinned != candidate)
            {
                continue;
            }
            nativeBackend = candidate;
            interfaceTable.getSnapshot = &NativeFileGetSnapshot;
            interfaceTable.applyControl = &NativeFileApplyControl;
            interfaceTable.registerD3D12Device = registerDevice;
            interfaceTable.registerD3D12Queue = registerQueue;
            interfaceTable.registerD3D12Swapchain = registerSwapchain;
            interfaceTable.unregisterD3D12Swapchain = unregisterSwapchain;
            interfaceTable.setFrontendAttached =
                &NativeFileSetFrontendAttached;
            break;
        }
        if (!nativeBackend)
        {
            ReleaseSRWLockExclusive(&gBackendConnectionLock);
            return false;
        }
    }

    gBackend = backend ? backend : nativeBackend;
    gBackendInterface = interfaceTable;
    gBackendKind.store(backend ? BackendKind::eAbi
        : BackendKind::eNativeFile, std::memory_order_release);
    gBackendConnected.store(true, std::memory_order_release);
    if (backend)
        gBackendInterface.setFrontendAttached(TRUE);
    ReleaseSRWLockExclusive(&gBackendConnectionLock);
    return true;
}

void DisconnectBackend() noexcept
{
    if (gBackendConnected.exchange(false, std::memory_order_acq_rel)
        && gBackendKind.load(std::memory_order_acquire) == BackendKind::eAbi
        && gBackendInterface.setFrontendAttached)
    {
        gBackendInterface.setFrontendAttached(FALSE);
    }
    gBackendInterface = {};
    gBackend = nullptr;
    gSampleFrameTelemetry = nullptr;
    gTelemetrySwapchain.store(0, std::memory_order_release);
    gTelemetrySwapchainTick.store(0, std::memory_order_release);
    gControlCompanionBackend.store(false, std::memory_order_release);
    gNativeMidpoint = {};
    gBackendKind.store(BackendKind::eNone, std::memory_order_release);
}
#endif

#if !defined(MFG_UNLOCK_SINGLE_MODULE_UI)
bool IsD3D12(reshade::api::device* device) noexcept
{
    return device && device->get_api() == reshade::api::device_api::d3d12;
}

#if defined(MFG_UNLOCK_V12_UNIVERSAL_UI)
void OnFinishPresent(reshade::api::command_queue*,
    reshade::api::swapchain* swapchain)
{
    if (!gSampleFrameTelemetry || !swapchain
        || !IsD3D12(swapchain->get_device()))
    {
        return;
    }

    const uintptr_t candidate = reinterpret_cast<uintptr_t>(swapchain);
    const uint64_t now = GetTickCount64();
    uintptr_t selected = gTelemetrySwapchain.load(std::memory_order_acquire);
    if (selected != candidate)
    {
        const uint64_t lastTick = gTelemetrySwapchainTick.load(
            std::memory_order_acquire);
        if (selected != 0 && now >= lastTick && now - lastTick < 2000)
            return;
        if (!gTelemetrySwapchain.compare_exchange_strong(selected, candidate,
                std::memory_order_acq_rel, std::memory_order_acquire)
            && selected != candidate)
        {
            return;
        }
    }
    gTelemetrySwapchainTick.store(now, std::memory_order_release);
    gSampleFrameTelemetry();
}
#endif

template <typename T>
T* NativeObject(reshade::api::api_object* object) noexcept
{
    return object ? reinterpret_cast<T*>(static_cast<uintptr_t>(
        object->get_native())) : nullptr;
}

void StorePresentationQueue(ID3D12CommandQueue* queue) noexcept
{
    if (queue)
        queue->AddRef();
    AcquireSRWLockExclusive(&gPresentationQueueLock);
    ID3D12CommandQueue* const previous = gPresentationQueue;
    gPresentationQueue = queue;
    ReleaseSRWLockExclusive(&gPresentationQueueLock);
    if (previous)
        previous->Release();
}

void RemovePresentationQueue(ID3D12CommandQueue* queue) noexcept
{
    ID3D12CommandQueue* removed = nullptr;
    AcquireSRWLockExclusive(&gPresentationQueueLock);
    if (!queue || gPresentationQueue == queue)
    {
        removed = gPresentationQueue;
        gPresentationQueue = nullptr;
    }
    ReleaseSRWLockExclusive(&gPresentationQueueLock);
    if (removed)
        removed->Release();
}

ID3D12CommandQueue* RetainPresentationQueue(
    reshade::api::device* expectedDevice) noexcept
{
    ID3D12CommandQueue* queue = nullptr;
    AcquireSRWLockShared(&gPresentationQueueLock);
    queue = gPresentationQueue;
    if (queue)
        queue->AddRef();
    ReleaseSRWLockShared(&gPresentationQueueLock);
    if (!queue || !IsD3D12(expectedDevice))
        return queue;

    ID3D12Device* queueDevice = nullptr;
    IUnknown* queueIdentity = nullptr;
    IUnknown* expectedIdentity = nullptr;
    ID3D12Device* const nativeExpected =
        NativeObject<ID3D12Device>(expectedDevice);
    const bool sameDevice = nativeExpected
        && SUCCEEDED(queue->GetDevice(IID_PPV_ARGS(&queueDevice)))
        && SUCCEEDED(queueDevice->QueryInterface(
            IID_PPV_ARGS(&queueIdentity)))
        && SUCCEEDED(nativeExpected->QueryInterface(
            IID_PPV_ARGS(&expectedIdentity)))
        && queueIdentity == expectedIdentity;
    if (expectedIdentity)
        expectedIdentity->Release();
    if (queueIdentity)
        queueIdentity->Release();
    if (queueDevice)
        queueDevice->Release();
    if (!sameDevice)
    {
        queue->Release();
        queue = nullptr;
    }
    return queue;
}

void OnInitDevice(reshade::api::device* device)
{
    if (IsD3D12(device))
        BackendRegisterDevice(NativeObject<ID3D12Device>(device));
}

void OnInitCommandQueue(reshade::api::command_queue* queue)
{
    if (!queue || !IsD3D12(queue->get_device()))
        return;
    ID3D12CommandQueue* const native =
        NativeObject<ID3D12CommandQueue>(queue);
    BackendRegisterQueue(native);
    if ((queue->get_type() & reshade::api::command_queue_type::graphics)
        != reshade::api::command_queue_type{})
    {
        StorePresentationQueue(native);
    }
}

void OnDestroyCommandQueue(reshade::api::command_queue* queue)
{
    if (queue && IsD3D12(queue->get_device()))
        RemovePresentationQueue(NativeObject<ID3D12CommandQueue>(queue));
}

void OnInitSwapchain(reshade::api::swapchain* swapchain, bool)
{
    if (!swapchain || !IsD3D12(swapchain->get_device()))
        return;
    ID3D12CommandQueue* const queue =
        RetainPresentationQueue(swapchain->get_device());
    if (queue)
    {
        BackendRegisterSwapchain(
            NativeObject<IDXGISwapChain>(swapchain), queue);
        queue->Release();
    }
}

void OnDestroySwapchain(reshade::api::swapchain* swapchain, bool)
{
    if (swapchain && IsD3D12(swapchain->get_device()))
    {
        BackendUnregisterSwapchain(
            NativeObject<IDXGISwapChain>(swapchain));
    }
}

void EnsureDefaultOverlayDock(reshade::api::effect_runtime*)
{
    const bool overlayVisited = gOverlayVisitedThisFrame;
    const bool overlayDocked = gOverlayDockedThisFrame;
    gOverlayVisitedThisFrame = false;
    gOverlayDockedThisFrame = false;

    if (!overlayVisited || overlayDocked || gDefaultDockInitialized)
        return;

    // ReShade invokes this event after registered overlay windows. Re-open the
    // already-created Home window to discover its public dock ID, then append
    // one empty Begin/End to our window with that ID queued. Dear ImGui
    // explicitly supports multiple Begin calls for one window in a frame.
    // This avoids depending on ReShade's private DockBuilder node numbering.
    ImGui::Begin(kReShadeHomeWindow, nullptr,
        ImGuiWindowFlags_NoFocusOnAppearing);
    const ImGuiID homeDockId = ImGui::GetWindowDockID();
    ImGui::End();
    if (homeDockId == 0)
        return;

    ImGui::SetNextWindowDockID(homeDockId, ImGuiCond_Always);
    ImGui::Begin(kOverlayTitle, nullptr,
        ImGuiWindowFlags_NoFocusOnAppearing);
    ImGui::End();
}

void RegisterRuntimeEvents()
{
#if defined(MFG_UNLOCK_V12_UNIVERSAL_UI)
    reshade::register_event<reshade::addon_event::finish_present>(
        OnFinishPresent);
#else
    reshade::register_event<reshade::addon_event::init_device>(OnInitDevice);
    reshade::register_event<reshade::addon_event::init_command_queue>(
        OnInitCommandQueue);
    reshade::register_event<reshade::addon_event::destroy_command_queue>(
        OnDestroyCommandQueue);
    reshade::register_event<reshade::addon_event::init_swapchain>(
        OnInitSwapchain);
    reshade::register_event<reshade::addon_event::destroy_swapchain>(
        OnDestroySwapchain);
#endif
    reshade::register_event<reshade::addon_event::reshade_overlay>(
        EnsureDefaultOverlayDock);
}

void UnregisterRuntimeEvents()
{
    reshade::unregister_event<reshade::addon_event::reshade_overlay>(
        EnsureDefaultOverlayDock);
#if defined(MFG_UNLOCK_V12_UNIVERSAL_UI)
    reshade::unregister_event<reshade::addon_event::finish_present>(
        OnFinishPresent);
#else
    reshade::unregister_event<reshade::addon_event::destroy_swapchain>(
        OnDestroySwapchain);
    reshade::unregister_event<reshade::addon_event::init_swapchain>(
        OnInitSwapchain);
    reshade::unregister_event<reshade::addon_event::destroy_command_queue>(
        OnDestroyCommandQueue);
    reshade::unregister_event<reshade::addon_event::init_command_queue>(
        OnInitCommandQueue);
    reshade::unregister_event<reshade::addon_event::init_device>(OnInitDevice);
    RemovePresentationQueue(nullptr);
#endif
}

bool ExistingMfgBackendPresent(HMODULE self) noexcept
{
    HMODULE modules[1024]{};
    DWORD bytes = 0;
    if (!K32EnumProcessModules(GetCurrentProcess(), modules,
            sizeof(modules), &bytes))
    {
        return true;
    }

    const DWORD count = std::min<DWORD>(bytes / sizeof(HMODULE),
        static_cast<DWORD>(std::size(modules)));
    for (DWORD index = 0; index < count; ++index)
    {
        HMODULE module = modules[index];
        if (!module || module == self)
            continue;
        if (GetProcAddress(module, "MfgUnlockRegisterD3D12Queue")
            || GetProcAddress(module, "MfgUnlockReShadeGetSnapshot"))
        {
            return true;
        }
    }
    return false;
}

#endif // ReShade graphics/event host

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
#if defined(MFG_UNLOCK_V12_UNIVERSAL_UI)
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
#endif
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
#if defined(MFG_UNLOCK_RESHADE_UI_CLIENT)
#if defined(MFG_UNLOCK_V12_UNIVERSAL_UI)
        const BOOL companionConfigPersisted = TRUE;
        const BOOL nativeConfigPersisted = accepted;
#else
        const BOOL companionConfigPersisted =
            PersistCompanionControl();
        const BOOL nativeConfigPersisted =
            !gIntegratedUniversalBackend.load(std::memory_order_acquire)
            || (ResolveNativeFilePaths()
                && NativeFileApplyControl(
                    static_cast<uint32_t>(gMultiplier),
                    !gFollowGameMode && gDynamicMode ? TRUE : FALSE,
                    static_cast<uint32_t>(gDynamicTargetFrameRate),
                    gDynamicExperimental56 ? TRUE : FALSE,
                    gGeneratedOnlyDebug ? TRUE : FALSE));
#endif
        gLastNativeConfigPersisted = companionConfigPersisted != FALSE
            && nativeConfigPersisted != FALSE;
#else
        gLastNativeConfigPersisted = true;
#endif
        gLastApplyAttempted = true;
        gLastApplyAccepted = accepted != FALSE;
        if (accepted) PersistSettings(runtime);
    }
}

void LoadSettings()
{
    gPresetSaveFailed = false;
    gPresentationSaveFailed = false;
    gDefaultDockInitialized = false;
    reshade::get_config_value(nullptr, kConfigSection,
        "DefaultDockInitialized", gDefaultDockInitialized);

    MfgUnlockReShadeSnapshot snapshot{};
    bool snapshotLoaded = BackendGetSnapshot(&snapshot) != FALSE;
#if defined(MFG_UNLOCK_RESHADE_UI_CLIENT)
    // Saved intent is independent of a transient status-file read failure.
    if (BackendUsesNativeFile())
    {
        MfgUnlockReShadeSnapshot saved{};
        if (ReadNativeControlSnapshot(&saved)) { snapshot = saved; snapshotLoaded = true; }
    }
#endif
    if (snapshotLoaded)
    {
        gFollowGameMode = snapshot.followGameMode != FALSE;
        gMultiplier = static_cast<int>(snapshot.desiredMultiplier);
        gDynamicMode = snapshot.dynamicMode != FALSE;
        gDlssgPreset = static_cast<int>(snapshot.dlssgPresetRequested);
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

#if defined(MFG_UNLOCK_V12_UNIVERSAL_UI)
    // The early-loaded core owns the universal control file. ReShade may
    // initialize much later, so stale ReShade.ini values must not overwrite
    // the request which was already consumed during Streamline startup.
    // No startup/migration write is allowed merely because status is absent.
    return;
#endif

    reshade::get_config_value(nullptr, kConfigSection,
        "FollowGameMode", gFollowGameMode);
    reshade::get_config_value(nullptr, kConfigSection,
        "Multiplier", gMultiplier);
    reshade::get_config_value(nullptr, kConfigSection,
        "DynamicMode", gDynamicMode);
    reshade::get_config_value(nullptr, kConfigSection,
        "DynamicTargetFrameRate", gDynamicTargetFrameRate);
    gDynamicLockToRefreshRate = gDynamicTargetFrameRate == 0;
    if (!gDynamicLockToRefreshRate)
        gDynamicCustomTargetFrameRate = gDynamicTargetFrameRate;
    reshade::get_config_value(nullptr, kConfigSection,
        "DynamicLockToRefreshRate", gDynamicLockToRefreshRate);
    reshade::get_config_value(nullptr, kConfigSection,
        "DynamicCustomTargetFrameRate", gDynamicCustomTargetFrameRate);
    gDynamicCustomTargetFrameRate = std::clamp(
        gDynamicCustomTargetFrameRate, 1, 1000);
    gDynamicTargetFrameRate = gDynamicLockToRefreshRate
        ? 0 : gDynamicCustomTargetFrameRate;
    reshade::get_config_value(nullptr, kConfigSection,
        "GeneratedOnlyDebug", gGeneratedOnlyDebug);
    ApplySettings(nullptr, false);
#if defined(MFG_UNLOCK_RESHADE_UI_CLIENT)
    // Migrate existing ReShade-only settings to the early companion file.
    // The bridge consumes this on the next public DLSS-G lookup, even when
    // ReShade itself initializes after the feature allocation boundary.
    gLastNativeConfigPersisted = PersistCompanionControl() != FALSE;
#endif
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

// Controls stay usable-looking while status is stale, so an open popup keeps
// its appearance across a missed read. Item-level disables inside still dim.
void BeginStaleDisabled(bool stale)
{
    ImGui::PushStyleVar(ImGuiStyleVar_DisabledAlpha, 1.0f);
    ImGui::BeginDisabled(stale);
    ImGui::PopStyleVar();
}

// Label | value grid shared by controls and status rows.
bool BeginGrid(const char* id)
{
    if (!ImGui::BeginTable(id, 2, ImGuiTableFlags_NoSavedSettings)) return false;
    ImGui::TableSetupColumn("##label", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFontSize() * 8.0f);
    ImGui::TableSetupColumn("##value", ImGuiTableColumnFlags_WidthStretch);
    return true;
}

void RowLabel(const char* label, bool control)
{
    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(0);
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

// The overlay draws this beside the menu key, after the panel.
const char* gFooterStatus = "";
Tone gFooterTone = Tone::Dim;

void SetFooter(const char* text, Tone tone)
{
    gFooterStatus = text;
    gFooterTone = tone;
}

// Diagnostics; shared by the universal and split panels.
void DrawHudlessDetection(const MfgUnlockReShadeSnapshot& snapshot)
{
    const auto text = [](const char* value) { return value[0] ? value : "waiting"; };
    const bool gameRecomposition = strcmp(snapshot.hudRecompositionState, "game-managed") == 0;
    ImGui::Text("UI recomposition: %s", gameRecomposition ? "on (the game's own)" : "off");
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("DLSS-G UI recomposition is left to the game; RTXMFG never enables it.\n"
            "An NVIDIA frame generation preset can still turn it on in the driver.");
    const bool layerFromHudless = strcmp(snapshot.hudRecompositionUi, "synthesized") == 0;
    ImGui::Text("HUD layer: %s", !snapshot.hudRecompositionVerified ? "not verified"
        : layerFromHudless ? "verified (from final colour minus HUDless)" : "verified (game UI buffer)");
    if (snapshot.hudlessDetectDiffersInMotion)
        ImGui::TextWrapped("HUDless matches when the camera is still but differs while it moves "
            "(likely motion blur).");
#if defined(MFG_UNLOCK_SINGLE_MODULE_UI)
    bool tint = hudless_visualizer::Enabled();
    if (ImGui::Checkbox("Tint pixels missing from HUDless", &tint)) hudless_visualizer::SetEnabled(tint);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Debug: paints magenta wherever the presented image differs from the\n"
            "game's HUDless image, i.e. what the HUDless image leaves out.\n"
            "Session only; changes presented pixels while enabled.");
    if (tint)
    {
        ImGui::Text("Tint: %s | %llu frames (%llu frame-paired)", hudless_visualizer::StateText(),
            static_cast<unsigned long long>(hudless_visualizer::FramesDrawn()),
            static_cast<unsigned long long>(hudless_visualizer::PairedFrames()));
        // Recomposition builds generated frames from the game's HUDless and UI
        // buffers, which never contain the tint.
        if (gameRecomposition)
            ImGui::TextWrapped("The game's UI recomposition is on, so the tint appears on rendered "
                "frames only (1 in N displayed frames at Nx).");
    }
#endif
    if (!snapshot.hudlessDetectEnabled)
    {
        ImGui::TextUnformatted("Disabled by [HUD] Detection=0");
        return;
    }
    ImGui::Text("Source: %s | route: %s", text(snapshot.hudlessDetectSource),
        text(snapshot.hudlessDetectRoute));
    ImGui::Text("Verdict: %s | probes: %u (%u conclusive)",
        text(snapshot.hudlessDetectVerdict), snapshot.hudlessDetectProbes,
        snapshot.hudlessDetectConclusive);
    if (snapshot.hudlessDetectProbes)
    {
        ImGui::Text("Last probe: identity %.1f%% | composite %.1f%% (%u/%u translucent) | best %s %.1f%%",
            snapshot.hudlessDetectIdentityPermille / 10.0,
            snapshot.hudlessDetectCompositePermille / 10.0,
            snapshot.hudlessDetectTranslucentTiles, snapshot.hudlessDetectInformativeTiles,
            text(snapshot.hudlessDetectBestHypothesis),
            snapshot.hudlessDetectBestPermille / 10.0);
    }
    if (snapshot.hudlessPresentMarkers)
        ImGui::Text("Frame pairing: by frame index | tags lead Present by %d (max %d) | last probe %s, still %.1f%%",
            snapshot.hudlessTagLead, snapshot.hudlessMaxTagLead,
            snapshot.hudlessDetectPaired ? "paired" : "unpaired", snapshot.hudlessDetectStillPermille / 10.0);
    else
        ImGui::TextUnformatted("Frame pairing: timing only (no present markers)");
    if (snapshot.hudlessDetectExtentWidth)
    {
        ImGui::Text("HUDless %ux%u fmt %u life %u | final %ux%u fmt %u",
            snapshot.hudlessDetectExtentWidth, snapshot.hudlessDetectExtentHeight,
            snapshot.hudlessDetectFormat, snapshot.hudlessDetectLifecycle,
            snapshot.hudlessDetectFinalWidth, snapshot.hudlessDetectFinalHeight,
            snapshot.hudlessDetectFinalFormat);
    }
    if (snapshot.hudlessDetectReason[0] && strcmp(snapshot.hudlessDetectReason, "none") != 0)
        ImGui::TextWrapped("Note: %s", snapshot.hudlessDetectReason);
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
            StatusDetail("DLSS-G preset", Tone::Good, "Original", "observed");
        else
            StatusDetail("DLSS-G preset", Tone::Pending, "Original", "waiting for the provider");
        return;
    }
    const uint32_t activePreset = snapshot.dlssgPresetSelectionFrozen
        ? snapshot.dlssgPresetLatched : snapshot.dlssgPresetRequested;
    char value[32]{};
    snprintf(value, sizeof(value), "Preset %s", DlssgPresetName(activePreset));
    if (activePreset == 0)
        StatusDetail("DLSS-G preset", Tone::Normal, "Game / driver", "managed by the game or driver");
    else if (activePreset == 3 && snapshot.dlssgPresetReadCount > 0 && !snapshot.dlssgPresetCAvailable)
        StatusDetail("DLSS-G preset", Tone::Pending, value, "needs DLSS-G 310.9.16; game / driver in use");
    else if (snapshot.dlssgPresetObservedValid && snapshot.dlssgPresetObserved == activePreset
        && snapshot.dlssgPresetOverrideReadCount > 0)
        StatusDetail("DLSS-G preset", Tone::Good, value, "override supplied");
    else if (snapshot.dlssgPresetOverrideFailure != 0)
        StatusDetail("DLSS-G preset", Tone::Bad, value, "override unavailable");
    else
        StatusDetail("DLSS-G preset", Tone::Pending, value, "waiting for the provider");
}

// Preset messages the user acts on.
void PresetNotes(const MfgUnlockReShadeSnapshot& snapshot)
{
    if (UiAmpere() && snapshot.ampereLegacySinglePreset)
    {
        Note(Tone::Dim, "This DLSS-G provider supports its original preset only.%s",
            snapshot.dlssgPresetRequested == 2 ? " Preset B stays saved for providers that support it." : "");
    }
    else if (snapshot.dlssgPresetSelectionFrozen
        && snapshot.dlssgPresetRequested != snapshot.dlssgPresetLatched)
    {
        if (snapshot.dlssgPresetRequested == 0)
            Note(Tone::Pending, "The game / driver preset is saved. Restart the game to use it.");
        else
            Note(Tone::Pending, "Preset %s is saved. Restart the game to use it.",
                DlssgPresetName(snapshot.dlssgPresetRequested));
    }
    const uint32_t activePreset = snapshot.dlssgPresetSelectionFrozen
        ? snapshot.dlssgPresetLatched : snapshot.dlssgPresetRequested;
    if (!(UiAmpere() && snapshot.ampereLegacySinglePreset) && activePreset == 3
        && snapshot.dlssgPresetReadCount > 0 && !snapshot.dlssgPresetCAvailable)
        Note(Tone::Dim, "Preset C needs DLSS-G 310.9.16 or newer (NVIDIA app). This game's"
            " frame generation keeps the game / driver preset.");
    if (gPresetSaveFailed)
        Note(Tone::Bad, "The preset selection could not be saved.");
}

// Preset control row. Updates snapshot.dlssgPresetRequested to the selection
// for the status row.
void PresetControlRow(MfgUnlockReShadeSnapshot& snapshot, bool statusStale)
{
#if defined(MFG_UNLOCK_RESHADE_UI_CLIENT) && MFG_UNLOCK_RUNTIME_GPU_SELECTION
    // Preset C needs DLSS-G 310.9.16 or newer (RTX 40 and, since 931, the RTX 30 port).
    const int maximumPreset = 3;
    if (!statusStale && snapshot.dlssgPresetRequested <= 3)
        gDlssgPreset = static_cast<int>(snapshot.dlssgPresetRequested);
    const int previous = gDlssgPreset;
    bool changed = false;
    static const char* options[] = {"Game / driver default", "Preset A", "Preset B", "Preset C (performance)"};
    const bool legacyPreset = UiAmpere() && snapshot.ampereLegacySinglePreset;
    ControlRow("DLSS-G preset");
    ImGui::BeginDisabled(legacyPreset);
    if (ImGui::BeginCombo("##dlssgpreset", legacyPreset ? "Original preset" : options[std::clamp(gDlssgPreset, 0, 3)]))
    {
        for (int preset = 0; preset <= maximumPreset; ++preset)
        {
            if (ImGui::Selectable(options[preset], gDlssgPreset == preset))
            {
                gDlssgPreset = preset;
                changed = preset != previous;
            }
        }
        ImGui::EndCombo();
    }
    ImGui::EndDisabled();
    HelpMarker("The DLSS frame generation model preset.\n"
        "Preset C is NVIDIA's lighter Frame Generation mode. It needs DLSS-G 310.9.16 or newer,\n"
        "from the NVIDIA app's DLSS override; other providers keep the game / driver preset.\n"
        "A change made after frame generation has started applies after a restart.");
    if (changed)
    {
        const BOOL saved = NativeFilePersistPreset(static_cast<uint32_t>(gDlssgPreset));
        gLastApplyAttempted = true;
        gLastApplyAccepted = saved != FALSE;
        gLastNativeConfigPersisted = saved != FALSE;
        gPresetSaveFailed = !saved;
        if (!saved) gDlssgPreset = previous;
    }
    snapshot.dlssgPresetRequested = static_cast<uint32_t>(gDlssgPreset);
#else
    (void)snapshot;
    (void)statusStale;
#endif
}

const char* ReflexPendingMessage(uint32_t status)
{
    switch (static_cast<reflex_control::Status>(status))
    {
    case reflex_control::Status::eWaitingForModule: return "Waiting for the game's Reflex plugin.";
    case reflex_control::Status::eUnsupportedModule: return "The loaded Reflex plugin version is unsupported.";
    case reflex_control::Status::eHookUnavailable: return "Reflex controls are unavailable for this plugin.";
    case reflex_control::Status::eIneligible: return "The Reflex limit is unavailable for the current FG runtime.";
    case reflex_control::Status::eWaitingForGameOptions: return "Waiting for game Reflex settings. Change the game's Reflex setting once to retry.";
    case reflex_control::Status::eWaitingForThread: return "Waiting for the game's Reflex settings thread.";
    case reflex_control::Status::eWaitingForAvailability: return "Waiting for Reflex availability.";
    case reflex_control::Status::eUnavailable: return "Reflex is unavailable.";
    case reflex_control::Status::eRestorePending: return "Restoring the game's Reflex limit...";
    case reflex_control::Status::eCallRejected: return "The game rejected the Reflex limit.";
    case reflex_control::Status::eUnknownShape: return "The game's Reflex settings cannot be safely replayed.";
    case reflex_control::Status::eStaleRoute: return "The Reflex plugin changed. Restart the game.";
    case reflex_control::Status::eBusy: return "Waiting for the next safe Reflex update.";
    default: return "Waiting for the game to accept the Reflex limit.";
    }
}

// Set by the control rows; read by the status rows in the same frame.
bool gPresentationChangedThisFrame = false;

// V-Sync and Reflex limit rows for the DLSS Latency and display grid.
void PresentationControlRows(const MfgUnlockReShadeSnapshot& snapshot, bool statusStale)
{
    gPresentationChangedThisFrame = false;
#if defined(MFG_UNLOCK_RESHADE_UI_CLIENT)
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
    const char* const vsyncModes[] = {"Game", "Off"};
    int vsyncChoice = gVsyncMode == 1 ? 1 : 0;
    ControlRow("V-Sync");
    if (ImGui::Combo("##vsync", &vsyncChoice, vsyncModes, 2))
    {
        gVsyncMode = vsyncChoice;
        changed = true;
    }
    HelpMarker("Off submits the game's frames without V-Sync.\n"
        "NVIDIA Control Panel V-Sync settings take priority.");
    const bool dynamic = snapshot.dynamicMode || snapshot.appliedDynamicMode;
    ControlRow("Reflex FPS limit");
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
        ImGui::TextDisabled("Off");
    }
    ImGui::EndDisabled();
    HelpMarker("Caps the game's frame rate through Reflex.\n"
        "Paused while Dynamic MFG is on; the saved limit is kept.");
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
#else
    (void)snapshot;
    (void)statusStale;
#endif
}

void PresentationNotes(const MfgUnlockReShadeSnapshot& snapshot)
{
#if defined(MFG_UNLOCK_RESHADE_UI_CLIENT)
    if (!BackendUsesNativeFile()) return;
    if ((snapshot.dynamicMode || snapshot.appliedDynamicMode) && gReflexFrameLimitFps > 0)
        Note(Tone::Dim, "The Reflex limit is paused while Dynamic MFG is on. The saved limit is kept.");
    if (gPresentationSaveFailed)
        Note(Tone::Bad, "The presentation settings could not be saved.");
#else
    (void)snapshot;
#endif
}

// V-Sync and Reflex readback for the DLSS Status grid.
void PresentationStatusRows(const MfgUnlockReShadeSnapshot& snapshot, bool statusStale)
{
#if defined(MFG_UNLOCK_RESHADE_UI_CLIENT)
    if (!BackendUsesNativeFile()) return;
    const bool changed = gPresentationChangedThisFrame;
    char detail[96]{};
    if (gVsyncMode == 1)
    {
        const bool current = !statusStale && !changed && snapshot.vsyncMode == 1;
        if (current && snapshot.vsyncPresentationObserved && snapshot.vsyncOverrideApplied)
        {
            snprintf(detail, sizeof(detail), "game interval %u submitted as 0", snapshot.vsyncOriginalInterval);
            StatusDetail("V-Sync", Tone::Good, "Off", detail);
        }
        else if (current && snapshot.vsyncPresentationObserved)
            StatusDetail("V-Sync", Tone::Good, "Off", "the game already presents without V-Sync");
        else if (current && snapshot.vsyncFailure
                == static_cast<uint32_t>(vsync_control::Failure::ePresentFailed))
            StatusDetail("V-Sync", Tone::Bad, "Off not applied", "the game's last Present was not accepted");
        else if (current)
            StatusDetail("V-Sync", Tone::Pending, "Off requested", "waiting for the game's swapchain");
        else
            StatusRow("V-Sync", Tone::Pending, "Applying...");
    }
    const bool dynamic = snapshot.dynamicMode || snapshot.appliedDynamicMode;
    if (gReflexFrameLimitFps > 0)
    {
        const uint32_t requestedUs = (1000000u + gReflexFrameLimitFps - 1u) / gReflexFrameLimitFps;
        char value[48]{};
        if (!dynamic && !statusStale && !changed && snapshot.reflexAppliedKnown && !snapshot.reflexLimitPending
            && !snapshot.reflexRestorePending && snapshot.reflexAppliedFrameLimitUs == requestedUs
            && snapshot.reflexStatus == static_cast<uint32_t>(reflex_control::Status::eApplied))
        {
            snprintf(value, sizeof(value), "%.2f FPS", 1000000.0 / snapshot.reflexAppliedFrameLimitUs);
            StatusDetail("Reflex limit", Tone::Good, value, "accepted");
        }
        else
        {
            snprintf(value, sizeof(value), "%d FPS requested", gReflexFrameLimitFps);
            if (snapshot.reflexRestorePending)
                StatusDetail("Reflex limit", Tone::Pending, value, "restoring the game's Reflex limit");
            else if (dynamic)
                StatusDetail("Reflex limit", Tone::Dim, value, "paused for Dynamic MFG");
            else
                StatusDetail("Reflex limit", Tone::Pending, value, statusStale
                    ? "Reflex status is temporarily unavailable." : ReflexPendingMessage(snapshot.reflexStatus));
        }
    }
    else if (snapshot.reflexRestorePending)
        StatusRow("Reflex limit", Tone::Pending, "Restoring the game's Reflex limit...");
#else
    (void)snapshot;
    (void)statusStale;
#endif
}

void DrawPresentationDebug(const MfgUnlockReShadeSnapshot& snapshot)
{
    ImGui::Text("V-Sync: requested=%u available=%u observed=%u applied=%u interval=%u->%u failure=%s",
        snapshot.vsyncMode, snapshot.vsyncControlAvailable, snapshot.vsyncPresentationObserved,
        snapshot.vsyncOverrideApplied, snapshot.vsyncOriginalInterval, snapshot.vsyncSubmittedInterval,
        vsync_control::FailureName(static_cast<vsync_control::Failure>(snapshot.vsyncFailure)));
    ImGui::Text("Reflex: available=%u accepted=%u limit=%u us pending=%u restore=%u status=%u result=%d",
        snapshot.reflexControlAvailable, snapshot.reflexAppliedKnown, snapshot.reflexAppliedFrameLimitUs,
        snapshot.reflexLimitPending, snapshot.reflexRestorePending, snapshot.reflexStatus,
        snapshot.reflexLastResult);
    ImGui::Text("Reflex module: %u.%u.%u generation=%llu hooks=0x%X",
        snapshot.reflexModuleVersionMajor, snapshot.reflexModuleVersionMinor,
        snapshot.reflexModuleVersionPatch, static_cast<unsigned long long>(snapshot.reflexModuleGeneration),
        snapshot.reflexHookMask);
}

#if defined(MFG_UNLOCK_SINGLE_MODULE_UI)
// Witcher 3 only. The fallback is always prepared there; the game's own
// HairWorks / Path Traced Hair settings turn hair on and off.
void DrawWitcherHair()
{
    const auto status = witcher_dots::ReadSnapshot();
    if (!status.applicable) return;
    Section("Path traced hair");
    const bool active = status.stage == witcher_dots::Stage::Active;
    if (BeginGrid("##hair"))
    {
        if (status.environmentOverride)
            StatusDetail("State", Tone::Dim, "Off", "RTXMFG_WITCHER_DOTS=0");
        else
        {
            // "active (tracing converted hair)" -> "Active" + "tracing converted hair".
            std::string text = witcher_dots::ActivityText(status);
            std::string detail;
            if (const size_t open = text.find(" ("); open != std::string::npos && text.back() == ')')
            {
                detail = text.substr(open + 2, text.size() - open - 3);
                text.resize(open);
            }
            if (!text.empty()) text[0] = static_cast<char>(toupper(static_cast<unsigned char>(text[0])));
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
            StatusDetail("State", tone, text.c_str(), detail.c_str());
            HelpMarker("Turn it on or off with HairWorks and Path Traced Hair in the game's graphics settings.");
            if (active || status.builds)
            {
                StatusRow("Traced hair", Tone::Normal, "%u", status.lastHairAgeMs < 2000 ? status.hairInstances : 0u);
                const std::string updates = Grouped(status.updates) + " updates";
                StatusDetail("Builds", Tone::Normal, Grouped(status.builds).c_str(), updates.c_str());
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
    if (!ImGui::CollapsingHeader("Hair diagnostics")) return;
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
        "%TEMP%\\RTXMFG-witcher3-device-removed-<pid>.txt. Costs a little GPU time while enabled.");
    if (reportSaveFailed) Note(Tone::Bad, "Could not save the crash report setting.");
    if (status.crashReportArmed) Note(Tone::Good, "Crash report armed for this session.");
    else if (status.crashReportRequested) Note(Tone::Pending, "Crash report starts with the next launch.");
}
#endif

#if defined(MFG_UNLOCK_SINGLE_MODULE_UI)
constexpr uint32_t kIntelVendor = 0x8086;
constexpr uint32_t kNvidiaVendor = 0x10de;

const char* VendorName(uint32_t vendor) noexcept
{
    switch (vendor)
    {
    case kNvidiaVendor: return "NVIDIA";
    case 0x1002: return "AMD";
    case kIntelVendor: return "Intel";
    default: return "Unknown";
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
    return active == "xess" ? "XeSS (Intel)" : active == "dlss" ? "DLSS (NVIDIA)" : "Off";
}

// Shown above the settings until the game turns frame generation on.
void FrameGenerationOffNote(const MfgUnlockReShadeSnapshot& snapshot)
{
    if (gXessUi.activeFrameGeneration != "none") return;
    const bool dlssOffered = snapshot.setOptionsSeen || snapshot.activeWrapperObserved;
    const bool xessOffered = gXessUi.hooksActive != FALSE;
    if (dlssOffered && xessOffered)
        Note(Tone::Pending, "Turn on frame generation in the game's settings (DLSS or XeSS).");
    else if (dlssOffered)
        Note(Tone::Pending, "Turn on frame generation in the game's settings (DLSS).");
    else if (xessOffered)
        Note(Tone::Pending, "Turn on frame generation in the game's settings (XeSS).");
    else
        Note(Tone::Pending, "Turn on DLSS or XeSS frame generation in the game's settings.");
}

// Game FG and GPU rows; familyDetail adds the DLSS GPU family.
void GameRows(const MfgUnlockReShadeSnapshot& snapshot, bool familyDetail)
{
    const bool off = gXessUi.activeFrameGeneration == "none";
    StatusRow("Game FG", off ? Tone::Dim : Tone::Normal, "%s", ActiveFrameGenerationName());
    const uint32_t vendor = GameGpuVendor();
    const std::string gpu = !vendor ? std::string() : gXessUi.adapter.empty()
        ? std::string(VendorName(vendor)) : gXessUi.adapter;
    const char* family = !familyDetail ? "" : snapshot.gpuFamily == 1u ? "Ada (RTX 40)"
        : snapshot.gpuFamily == 2u ? "Ampere (RTX 30)" : "";
    if (!gpu.empty())
        StatusDetail("GPU", Tone::Normal, gpu.c_str(), family);
    else if (familyDetail && family[0])
        StatusRow("GPU", Tone::Normal, "%s", family);
    else if (familyDetail)
        StatusRow("GPU", Tone::Pending, "Waiting for a supported game GPU");
}

// XeSS frame generation: status only. beta.8: XeSS MFG runs under OptiScaler, whose menu owns its
// controls (the frame generation ratio and the MFG model). Without OptiScaler the game's XeSS frame
// generation stays at Intel's 2x and RTXMFG leaves it untouched.
void DrawXessPanel(const MfgUnlockReShadeSnapshot& snapshot, bool statusStale)
{
    const auto& x = gXessUi;
    static const char* models[] = {"MFG Unlocker multi-frame port", "Intel 2x model"};

    FrameGenerationOffNote(snapshot);
    Section("Frame generation");
    if (!x.available)
        Note(Tone::Pending, "XeSS frame generation support is unavailable in this build.");
    else if (!x.optiScaler)
        Note(Tone::Pending, "XeSS multi-frame generation needs OptiScaler, with XeFG as its frame "
            "generation output. Without it the game's XeSS frame generation runs at Intel's standard 2x.");
    else
        Note(Tone::Normal, "XeSS multi-frame generation runs through OptiScaler. Set the frame "
            "generation ratio and the MFG model in OptiScaler's menu.");

    Section("Display");
    int vsync = snapshot.vsyncMode == 1 ? 1 : 0;
    bool vsyncChanged = false;
    BeginStaleDisabled(statusStale);
    if (BeginGrid("##xessdisplay"))
    {
        const char* vsyncModes[] = {"Game", "Off"};
        ControlRow("V-Sync");
        vsyncChanged = ImGui::Combo("##xessvsync", &vsync, vsyncModes, IM_ARRAYSIZE(vsyncModes));
        HelpMarker("Off submits the game's frames without V-Sync.");
        ImGui::EndTable();
    }
    if (vsyncChanged && !NativeFilePersistPresentation(static_cast<uint32_t>(vsync), snapshot.reflexFrameLimitFps))
        Note(Tone::Bad, "The V-Sync setting could not be saved.");
    ImGui::EndDisabled();

    DrawWitcherHair();

    Section("Status");
    char value[64]{}, detail[96]{};
    if (BeginGrid("##xessstatus"))
    {
        GameRows(snapshot, false);
        if (x.available && x.optiScaler)
        {
            if (xess_ui::AppliedKnown(x))
            {
                snprintf(value, sizeof(value), "%ux", x.applied);
                snprintf(detail, sizeof(detail), "%u generated frames capacity", x.capacity);
                StatusDetail("Applied", Tone::Good, value, detail);
            }
            else if (x.initialized)
                StatusRow("Applied", Tone::Pending, "Not verified");
            if (x.modelActive == 1 || x.modelActive == 2)
                StatusRow("Model", Tone::Normal, "%s", models[x.modelActive - 1]);
        }
        if (vsync == 1)
        {
            const auto result = !statusStale && !vsyncChanged
                ? xess_ui::VsyncResult(snapshot.vsyncControlAvailable != FALSE,
                    snapshot.vsyncPresentationObserved != FALSE, snapshot.vsyncOverrideApplied != FALSE,
                    snapshot.vsyncOriginalInterval, snapshot.vsyncSubmittedInterval)
                : xess_ui::VsyncReadback::Pending;
            switch (result)
            {
            case xess_ui::VsyncReadback::Overridden:
                snprintf(detail, sizeof(detail), "game interval %u submitted as 0", snapshot.vsyncOriginalInterval);
                StatusDetail("V-Sync", Tone::Good, "Off", detail);
                break;
            case xess_ui::VsyncReadback::AlreadyOff:
                StatusDetail("V-Sync", Tone::Good, "Off", "the game already presents without V-Sync");
                break;
            default:
                StatusDetail("V-Sync", Tone::Pending, "Off requested", "presentation override is not verified");
                break;
            }
        }
        ImGui::EndTable();
    }
    if (x.available && x.optiScaler && !x.status.empty()) Note(Tone::Dim, "%s", x.status.c_str());
    const uint32_t vendor = GameGpuVendor();
    const bool dlssOffered = snapshot.setOptionsSeen || snapshot.activeWrapperObserved;
    if (vendor && vendor != kNvidiaVendor)
        Note(Tone::Dim, "DLSS multi-frame generation needs an NVIDIA RTX GPU.");
    else if (dlssOffered && gXessUi.gpuVendorId == kNvidiaVendor && !xess_ui::DlssMfgFamily(snapshot.gpuFamily))
        Note(Tone::Dim, "DLSS multi-frame generation is unavailable on this GPU.");

    Section("Advanced");
    if (x.available && ImGui::CollapsingHeader("XeSS diagnostics"))
    {
        ImGui::Text("OptiScaler %s | hooks %s | initialized %s | enabled %s",
            x.optiScaler ? "detected" : "not detected", x.hooksActive ? "active" : "waiting",
            x.initialized ? "yes" : "no", x.enabled ? "yes" : "no");
        ImGui::Text("Provider frames %u | model %u", x.framesPresented, x.modelActive);
        if (!x.runtimeFailure.empty()) ImGui::TextWrapped("Runtime: %s", x.runtimeFailure.c_str());
    }
    DrawWitcherHairDiagnostics();
}
#endif

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
#if defined(MFG_UNLOCK_RESHADE_UI_CLIENT)
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
#endif
    if (!haveSnapshot)
    {
        if (nativeFile)
        {
            SetFooter("Waiting for status", Tone::Pending);
            Note(Tone::Pending, "Waiting for current backend status. Saved settings are preserved.");
            return;
        }
        SetFooter("Not connected", Tone::Bad);
#if defined(MFG_UNLOCK_RESHADE_UI_CLIENT)
        Note(Tone::Bad, MFG_PRODUCT " backend is not loaded.");
#if defined(MFG_UNLOCK_SINGLE_MODULE_UI)
        ImGui::TextWrapped("The integrated core is unavailable. Check the "
            "startup log and restart the game with one " MFG_PRODUCT " module.");
#else
        ImGui::TextWrapped("The UI loaded correctly. Install "
            MFG_PRODUCT ".asi through Ultimate ASI Loader and fully "
            "restart the game.");
#endif
#else
        ImGui::TextUnformatted("Native bridge snapshot is unavailable.");
#endif
        return;
    }
    if (nativeFile)
        SetFooter(statusStale ? "Paused" : "Live", statusStale ? Tone::Pending : Tone::Good);
    else
        SetFooter("", Tone::Dim);

#if defined(MFG_UNLOCK_SINGLE_MODULE_UI)
    {
        const bool dlssOffered = snapshot.setOptionsSeen || snapshot.activeWrapperObserved;
        if (xess_ui::SettingsSection(gXessUi, gLastGameFrameGeneration, dlssOffered, snapshot.gpuFamily)
            == xess_ui::Section::Xess)
        {
            DrawXessPanel(snapshot, statusStale);
            return;
        }
        FrameGenerationOffNote(snapshot);
        const uint32_t vendor = GameGpuVendor();
        if (vendor && vendor != kNvidiaVendor)
            Note(Tone::Dim, "DLSS multi-frame generation needs an NVIDIA RTX GPU.");
    }
#endif
    if (snapshot.gpuFamily == 3u)
    {
        Note(Tone::Pending, "The game changed graphics adapters. Restart the game to select a new GPU.");
        return;
    }

    MfgUnlockReShadeSnapshot presetView = snapshot;
    if (std::strcmp(snapshot.patchRoute, "remix") == 0)
    {
        Section("Frame generation");
        Note(Tone::Normal, "RTX Remix detected. Frame generation and its multiplier are managed "
            "in the RTX Remix menu.");
        BeginStaleDisabled(statusStale);
        if (BeginGrid("##remixfg"))
        {
            PresetControlRow(presetView, statusStale);
            ImGui::EndTable();
        }
        ImGui::EndDisabled();
        PresetNotes(presetView);
        Section("Status");
        if (BeginGrid("##remixstatus"))
        {
            StatusRow("Provider patch", snapshot.backportReadyAtCreate ? Tone::Good : Tone::Pending, "%s",
                snapshot.backportReadyAtCreate ? "Ready at feature creation" : "Waiting for frame generation");
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

    Section("Frame generation");
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
            static const char* ampereModes[]{"Off", "2x FG", "3x MFG", "4x MFG",
                "5x MFG (experimental)", "6x MFG (experimental)"};
            const char* dynamicLabel = ui_dynamic_mfg::Label(dynamicAvailability);
            const char* selectedMode = gFollowGameMode ? "Follow game"
                : gDynamicMode ? dynamicLabel : ampereModes[std::clamp(gMultiplier, 1, 6) - 1];
            ControlRow("Mode");
            if (ImGui::BeginCombo("##framegeneration", selectedMode))
            {
                if (ImGui::Selectable("Follow game", gFollowGameMode))
                { gFollowGameMode = true; gDynamicMode = false; settingsChanged = true; }
                for (int value = 1; value <= 6; ++value)
                {
                    ImGui::BeginDisabled(value > safeMaximumMultiplier);
                    if (ImGui::Selectable(ampereModes[value - 1], !gFollowGameMode && !gDynamicMode && gMultiplier == value)
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
            const char* dynamicLabel = ui_dynamic_mfg::Label(dynamicAvailability);
            const char* currentMode = !snapshot.bridgeReady
                ? "Unavailable"
                : gFollowGameMode
                ? "Follow game"
                : gDynamicMode
                    ? dynamicLabel
                    : fixedModes[
                        std::clamp(gMultiplier, 2, 6) - 2];
            ControlRow("MFG mode");
            ImGui::BeginDisabled(!snapshot.bridgeReady);
            if (ImGui::BeginCombo("##mfgmode", currentMode))
            {
                if (ImGui::Selectable("Follow game", gFollowGameMode))
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
        HelpMarker("Follow game uses the game's own frame generation setting.\n"
            "Dynamic adjusts frame generation toward a target frame rate; it does not set a hard FPS limit.");
        if (!gFollowGameMode && gDynamicMode)
        {
            ImGui::BeginDisabled(!dynamicSupported);
            ControlRow("Dynamic target");
            if (ImGui::Checkbox("Display refresh rate", &gDynamicLockToRefreshRate))
                settingsChanged = true;
            if (!gDynamicLockToRefreshRate)
            {
                ControlRow("Target FPS");
                ImGui::DragInt("##dynamictarget", &gDynamicCustomTargetFrameRate, 1.0f, 1, 1000, "%d FPS");
                if (ImGui::IsItemDeactivatedAfterEdit())
                    settingsChanged = true;
            }
            ImGui::EndDisabled();
        }
        PresetControlRow(presetView, statusStale);
        ImGui::EndTable();
    }
    if (!UiAmpere())
    {
        if (dynamicUnsupported)
            Note(Tone::Dim, "Dynamic MFG is unavailable with the current limit or runtime. "
                "Its full frame-generation range must fit the limit.");
        else if (!gFollowGameMode && gDynamicMode && !dynamicSupported)
            Note(Tone::Pending, "Dynamic selection is saved; waiting for runtime support.");
        if (!gFollowGameMode && !gDynamicMode
            && (!snapshot.bridgeReady || gMultiplier > safeMaximumMultiplier))
            Note(Tone::Pending, "Saved MFG request: %dx (unavailable in this session)", gMultiplier);
    }
    if (gLastApplyAttempted && !gPresetSaveFailed
        && (!gLastApplyAccepted || !gLastNativeConfigPersisted))
    {
        if (gLastApplyAccepted)
            Note(Tone::Bad, "Request accepted, but the configuration was not saved.");
        else
            Note(Tone::Bad, "The selection could not be applied or saved.");
    }
    PresetNotes(presetView);

    Section("Latency and display");
    if (BeginGrid("##dlsslatency"))
    {
        PresentationControlRows(snapshot, statusStale);
        ImGui::EndTable();
    }
    PresentationNotes(snapshot);
    ImGui::EndDisabled();

#if defined(MFG_UNLOCK_SINGLE_MODULE_UI)
    DrawWitcherHair();
#endif

    Section("Status");
    char value[64]{}, detail[128]{};
    if (BeginGrid("##dlssstatus"))
    {
#if defined(MFG_UNLOCK_SINGLE_MODULE_UI)
        GameRows(snapshot, true);
#else
        StatusRow("GPU", snapshot.gpuFamily ? Tone::Normal : Tone::Pending, "%s",
            snapshot.gpuFamily == 2u ? "Ampere (RTX 30)"
                : snapshot.gpuFamily == 1u ? "Ada (RTX 40)" : "Waiting for a supported game GPU");
#endif
        const bool fgKnown = snapshot.setOptionsSeen && snapshot.setOptionsAccepted;
        snprintf(detail, sizeof(detail), "game intent: %s", !snapshot.setOptionsSeen ? "unknown"
            : snapshot.gameFrameGenerationOn ? "on" : "off");
        StatusDetail("FG state", !fgKnown ? Tone::Dim : snapshot.appliedFrameGenerationOn ? Tone::Good : Tone::Normal,
            !fgKnown ? "Unknown" : snapshot.appliedFrameGenerationOn ? "On" : "Off", detail);
        if (snapshot.followGameMode)
            StatusRow("Requested", Tone::Normal, "Follow game");
        else if (snapshot.appliedRevision == 0 || snapshot.dynamicMode != snapshot.appliedDynamicMode
            || (!snapshot.dynamicMode && snapshot.desiredMultiplier != snapshot.appliedMultiplier))
            StatusRow("Requested", Tone::Pending, snapshot.dynamicMode ? "Dynamic" : "%ux",
                snapshot.desiredMultiplier);
        if (snapshot.appliedRevision != 0)
        {
            snprintf(value, sizeof(value), snapshot.appliedDynamicMode ? "Dynamic, %ux" : "%ux",
                snapshot.appliedMultiplier);
            StatusRow("Applied", snapshot.appliedFrameGenerationOn ? Tone::Good : Tone::Normal, "%s", value);
        }
        const char* nvidiaList = !snapshot.nvidiaCompatibilityResolved ? "NVIDIA list unavailable"
            : snapshot.nvidiaCompatibilityTier == 2 ? "NVIDIA lists no MFG for this game"
            : snapshot.nvidiaCompatibilityTier != 4 && snapshot.nvidiaCompatibilityTier != 6
                ? "game not on NVIDIA's list" : nullptr;
        if (!nvidiaList)
        {
            snprintf(detail, sizeof(detail), "NVIDIA lists %ux", snapshot.nvidiaCompatibilityTier);
            nvidiaList = detail;
        }
        if (wrapperControlUnavailable)
            StatusDetail("Limit", Tone::Bad, "Unavailable", "the current wrapper has no verified MFG override");
        else if (snapshot.activeWrapperObserved)
        {
            snprintf(value, sizeof(value), "%dx%s", safeMaximumMultiplier,
                snapshot.compatibilityFallback ? " (safe fallback)" : "");
            StatusDetail("Limit", Tone::Normal, value, nvidiaList);
        }
        else
            StatusDetail("Limit", Tone::Pending, "Waiting", "for an active DLSS-G wrapper");
        char limitHelp[256]{};
        if (snapshot.nvidiaCompatibilityTier == 2)
            snprintf(limitHelp, sizeof(limitHelp),
                "NVIDIA lists ordinary Frame Generation only. The mod keeps its override limit at 2x.");
        else if (snapshot.nvidiaPolicyCeilingMultiplier > static_cast<uint32_t>(safeMaximumMultiplier))
            snprintf(limitHelp, sizeof(limitHelp),
                "The listed %ux ceiling requires a compatible runtime. The current limit is %dx.",
                snapshot.nvidiaPolicyCeilingMultiplier, safeMaximumMultiplier);
        else
            snprintf(limitHelp, sizeof(limitHelp),
                "The highest mode the current DLSS-G runtime supports, and the ceiling\n"
                "NVIDIA's override list gives this game.");
        HelpMarker(limitHelp);
        const bool fpsSampleCurrent = snapshot.realFpsMilli > 0
            && snapshot.fpsSampleAgeMs <= 2000;
        if (snapshot.appliedFrameGenerationOn && fpsSampleCurrent)
        {
            if (snapshot.dlssFpsMilli > 0)
                StatusRow("FPS", Tone::Normal, "%.1f real, %.1f DLSS",
                    static_cast<double>(snapshot.realFpsMilli) / 1000.0,
                    static_cast<double>(snapshot.dlssFpsMilli) / 1000.0);
            else
                StatusRow("FPS", Tone::Normal, "%.1f real, DLSS unavailable",
                    static_cast<double>(snapshot.realFpsMilli) / 1000.0);
        }
#if defined(MFG_UNLOCK_V12_UNIVERSAL_UI)
        else if (snapshot.appliedFrameGenerationOn && gSampleFrameTelemetry)
        {
            StatusRow("FPS", Tone::Dim, "Measuring...");
        }
#endif
        if (!gFollowGameMode && gDynamicMode)
        {
            if (gDynamicLockToRefreshRate) snprintf(value, sizeof(value), "Display refresh rate");
            else snprintf(value, sizeof(value), "%d FPS", gDynamicCustomTargetFrameRate);
            if (!snapshot.appliedDynamicTargetValid)
                StatusDetail("Dynamic target", Tone::Pending, value, "waiting for an accepted Dynamic request");
            else
            {
                if (snapshot.appliedDynamicTargetFrameRate == 0.0f)
                    snprintf(detail, sizeof(detail), "applied: display refresh rate");
                else
                    snprintf(detail, sizeof(detail), "applied: %.2f FPS", snapshot.appliedDynamicTargetFrameRate);
                StatusDetail("Dynamic target", Tone::Good, value, detail);
            }
        }
        PresetStatusRow(presetView);
        PresentationStatusRows(snapshot, statusStale);
        ImGui::EndTable();
    }
    if (snapshot.statusProtocolVersion != MFG_STATUS_VERSION_NUMBER)
    {
#if defined(MFG_UNLOCK_SINGLE_MODULE_UI)
        Note(Tone::Bad, "Restart with the matching single-module build. "
            "The current status protocol is older than this UI expects.");
#else
        Note(Tone::Bad, "Update RTX40MFG.asi and RTX40MFGCore.dll "
            "together; the loaded core uses an older protocol.");
#endif
    }
    else if (!snapshot.bridgeReady)
    {
        if (wrapperControlUnavailable)
            Note(Tone::Pending, "Frame generation stays under the game's control. "
                "This wrapper has no verified MFG override support.");
        else if (snapshot.intervalValidSamples && !snapshot.setOptionsSeen)
            Note(Tone::Pending, "DLSS-G activity observed; waiting for the game's FG control calls.");
        else Note(Tone::Pending, "%s", snapshot.universalRouteFailure
            == static_cast<uint32_t>(universal_route_policy::Failure::eAwaitingOptionsRequest)
            ? "Waiting for the game to request frame generation."
            : "Waiting for an active DLSS-G pipeline.");
    }
    if (snapshot.streamlineRebuildRequired
        || snapshot.pipelineMayPredateDetour)
    {
        Note(Tone::Pending, "Toggle frame generation off then on, or restart "
            "the game, to recreate the pipeline with this selection.");
    }

    Section("Advanced");
#if defined(MFG_UNLOCK_V12_UNIVERSAL_UI)
    if (ImGui::CollapsingHeader("Diagnostics"))
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
        if (UiAmpere() && ImGui::TreeNode("Ampere"))
        {
            ImGui::Text("Ampere SM86 | D3D12 | FG preset: %s",
                snapshot.ampereLegacySinglePreset ? "Original preset"
                    : DlssgPresetName(snapshot.dlssgPresetSelectionFrozen
                        ? snapshot.dlssgPresetLatched : snapshot.dlssgPresetRequested));
            ImGui::Text("Ampere provider: %s | Preset query: %s",
                snapshot.ampereProgramReadyMfg ? "MFG program prepared" : "pending",
                snapshot.presetQueries ? "observed" : "pending");
            ImGui::Text("Kernel program: %s | Cache status: %u", snapshot.ampereKernelImage == 2 ? "native SM86"
                : snapshot.ampereKernelImage == 1 ? "SM86 PTX" : "pending", snapshot.ampereNativeCacheStatus);
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
                    (snapshot.ampereStartupFailureMask & 0x10u) ? "D3D12 device; " : "",
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
                ImGui::Text("Trace: %%TEMP%%\\%s", snapshot.intervalLogFile);
            ImGui::TreePop();
        }
        if (ImGui::TreeNode("HUDless detection"))
        {
            DrawHudlessDetection(snapshot);
            ImGui::TreePop();
        }
    }
#if defined(MFG_UNLOCK_SINGLE_MODULE_UI)
    DrawWitcherHairDiagnostics();
#endif
#else
    ImGui::BeginDisabled(statusStale);
    if (ImGui::CollapsingHeader("Debug"))
    {
        if (!UiAmpere())
        {
            if (ImGui::Checkbox("Generated frames only",
                    &gGeneratedOnlyDebug))
            {
                settingsChanged = true;
            }
        }
        ImGui::Separator();
        ImGui::TextUnformatted("Core and bridge");
        ImGui::Text("Core loaded: %s | Protocol: %u",
            YesNo(snapshot.loaderCoreImported),
            snapshot.statusProtocolVersion);
        ImGui::Text("Bridge: %s | Route: %s",
            snapshot.bridgeReady ? "connected" : "waiting",
            snapshot.patchRoute[0] ? snapshot.patchRoute : "pending");
#if defined(MFG_UNLOCK_RESHADE_UI_CLIENT) \
    && !defined(MFG_UNLOCK_V12_UNIVERSAL_UI)
        const BackendKind backendKind = gBackendKind.load(
            std::memory_order_acquire);
        const char* backendLabel = backendKind == BackendKind::eAbi
            ? "exported ABI" : backendKind == BackendKind::eNativeFile
                ? "status file" : "none";
        ImGui::Text("UI backend: %s | Companion: %s | Integrated: %s",
            backendLabel,
            gControlCompanionBackend.load(std::memory_order_acquire)
                ? "yes" : "no",
            gIntegratedUniversalBackend.load(std::memory_order_acquire)
                ? "yes" : "no");
#endif
#if !defined(MFG_UNLOCK_V12_UNIVERSAL_UI)
        ImGui::Text("Startup: profile %u | failure %u | mask 0x%08X",
            snapshot.startupProfile,
            snapshot.startupFailure,
            snapshot.startupValidationMask);
        ImGui::Text("Central hook: %s | clean snapshot: %s | worker: %s",
            YesNo(snapshot.centralHookInstalled),
            YesNo(snapshot.startupSnapshotClean),
            YesNo(snapshot.workerIdentityCertified));
        ImGui::Text("Early host: %s | DLL notify: %s | loader hooks: %s",
            YesNo(snapshot.transportEarlyHostCertified),
            YesNo(snapshot.dllNotificationRegistered),
            YesNo(snapshot.pluginLoaderHooksInstalled));
        ImGui::Text("Pre-entry route: %s | owner published: %s",
            YesNo(snapshot.preEntryRouteInstalled),
            YesNo(snapshot.selectedOwnerPublished));
        ImGui::Text("Pre-entry attempts: %llu | successes: %llu | failures: %llu",
            static_cast<unsigned long long>(snapshot.preEntryRouteAttempts),
            static_cast<unsigned long long>(snapshot.preEntryRouteSuccesses),
            static_cast<unsigned long long>(snapshot.preEntryRouteFailures));
#endif
        ImGui::Text("Main resolver: %s | loader discovery: %s (%llu calls)",
            YesNo(snapshot.mainResolverDiscoveryInstalled),
            YesNo(snapshot.streamlineLoaderDiscoveryInstalled),
            static_cast<unsigned long long>(
                snapshot.streamlineLoaderDiscoveryCalls));

        ImGui::Separator();
        ImGui::TextUnformatted("Compatibility");
        ImGui::Text("NVIDIA profile: %s | status: %d | resolved: %s",
            snapshot.nvidiaProfileName[0]
                ? snapshot.nvidiaProfileName : "not available",
            snapshot.nvidiaProfileStatus,
            YesNo(snapshot.nvidiaCompatibilityResolved));
        if (snapshot.nvidiaCompatibilityTier == 4
            || snapshot.nvidiaCompatibilityTier == 6)
        {
            ImGui::Text("NVIDIA MFG override ceiling: %uX",
                snapshot.nvidiaCompatibilityTier);
        }
        else if (snapshot.nvidiaCompatibilityTier == 2)
        {
            ImGui::TextUnformatted("NVIDIA MFG override: not listed for this game");
        }
        else
        {
            ImGui::TextUnformatted("NVIDIA MFG override ceiling: unknown title");
        }
        ImGui::Text("Manifest entries: %u | Policy maximum: %uX",
            snapshot.nvidiaCompatibilityManifestEntries,
            snapshot.nvidiaPolicyCeilingMultiplier);
        ImGui::Text("Available maximum: %uX | Wrapper maximum: %uX",
            safeMaximumMultiplier,
            snapshot.wrapperNativeMaximumMultiplier);
        ImGui::Text("Wrapper compiled output: %u generated frames",
            snapshot.wrapperCompiledMaximumGeneratedFrames);
        if (snapshot.activeWrapperObserved)
        {
            ImGui::Text("Wrapper: %s %u.%u.%u.%u",
                snapshot.activeWrapperUsesNvidiaOta
                    ? "NVIDIA OTA" : "game-provided",
                snapshot.activeWrapperVersionMajor,
                snapshot.activeWrapperVersionMinor,
                snapshot.activeWrapperVersionBuild,
                snapshot.activeWrapperVersionPrivate);
        }
        else
        {
            ImGui::TextUnformatted("Wrapper: waiting");
        }
        ImGui::Text("Fallback: %s | reason: %u | request limited: %s",
            YesNo(snapshot.compatibilityFallback),
            snapshot.compatibilityReason,
            YesNo(snapshot.requestedMultiplierLimited));

        ImGui::Separator();
        ImGui::TextUnformatted("Control and lifecycle");
    ImGui::Text("Dynamic availability: %s",
        !dynamicCapabilityKnown ? "checking"
            : dynamicSupported ? "supported" : "unsupported");
    ImGui::Text("Runtime FG V-Sync capability: %s", !snapshot.fgVsyncSupportKnown
        ? "unknown" : snapshot.fgVsyncSupported ? "supported" : "unavailable");
        DrawPresentationDebug(snapshot);
    if (snapshot.followGameMode)
    {
        ImGui::Text("Requested: follow game  Applied: %s %ux",
            snapshot.appliedRevision != 0
                ? (snapshot.appliedDynamicMode ? "dynamic" : "fixed")
                : "none",
            snapshot.appliedMultiplier);
    }
    else
    {
        ImGui::Text("Requested: %s %ux  Applied: %s %ux",
            snapshot.dynamicMode ? "dynamic" : "fixed",
            snapshot.desiredMultiplier,
            snapshot.appliedRevision != 0
                ? (snapshot.appliedDynamicMode ? "dynamic" : "fixed")
                : "none",
            snapshot.appliedMultiplier);
    }
    if (snapshot.dynamicTargetFrameRate == 0)
    {
        ImGui::TextUnformatted(
            "Saved Dynamic target: display refresh rate");
    }
    else
    {
        ImGui::Text("Saved Dynamic target: %u FPS",
            snapshot.dynamicTargetFrameRate);
    }
    ImGui::Text("Backend request revision: %llu | Applied revision: %llu",
        static_cast<unsigned long long>(snapshot.desiredRevision),
        static_cast<unsigned long long>(snapshot.appliedRevision));
    ImGui::Text("Generated-only desired/applied: %s/%s",
        YesNo(snapshot.generatedOnlyDebug),
        YesNo(snapshot.appliedGeneratedOnlyDebug));
    ImGui::Text("Use refresh as target: %s | custom target: %d FPS",
        gDynamicLockToRefreshRate ? "yes" : "no",
        gDynamicCustomTargetFrameRate);
    ImGui::Text("Last UI apply: attempted %s | accepted %s | saved %s",
        gLastApplyAttempted ? "yes" : "no",
        gLastApplyAccepted ? "yes" : "no",
        gLastNativeConfigPersisted ? "yes" : "no");
    ImGui::Text("FG applied/intent: %s/%s | rebuild: %s | pipeline predates detour: %s",
        YesNo(snapshot.appliedFrameGenerationOn),
        YesNo(snapshot.gameFrameGenerationOn),
        YesNo(snapshot.streamlineRebuildRequired),
        YesNo(snapshot.pipelineMayPredateDetour));

    ImGui::Separator();
    ImGui::TextUnformatted("Hooks and synthesis");
    ImGui::Text("SetOptions entry: %s | control path: %s | fallback: %s",
        YesNo(snapshot.setOptionsEntryDetourCurrent),
        YesNo(snapshot.setOptionsControlPathReady),
        YesNo(snapshot.setOptionsResolverFallbackActive));
    ImGui::Text("SetOptions fallback calls: %llu | seen: %s | accepted: %s",
        static_cast<unsigned long long>(
            snapshot.setOptionsResolverFallbackCalls),
        YesNo(snapshot.setOptionsSeen),
        YesNo(snapshot.setOptionsAccepted));
    ImGui::Text("GetState entry: %s | Create entry: %s | Evaluate entry: %s",
        YesNo(snapshot.getStateEntryDetourCurrent),
        YesNo(snapshot.ngxCreateEntryDetourCurrent),
        YesNo(snapshot.ngxEvaluateEntryDetourCurrent));
    ImGui::Text("Create program ready: %s | synthesis: %s",
        YesNo(snapshot.backportReadyAtCreate),
        YesNo(snapshot.perSampleSynthesisReady));
    ImGui::Text("High-capability publication allowed: %s",
        YesNo(snapshot.highCapabilityPublicationAllowed));
#if defined(MFG_UNLOCK_RESHADE_UI_CLIENT) \
    && !defined(MFG_UNLOCK_V12_UNIVERSAL_UI)
    ImGui::Text("Midpoint source: available %s | required %s | ready %s",
        gNativeMidpoint.available ? "yes" : "no",
        gNativeMidpoint.required ? "yes" : "no",
        gNativeMidpoint.ready ? "yes" : "no");
    ImGui::Text("Midpoint route: public %s | adapter %s | fallback %s",
        gNativeMidpoint.publicRouteReady ? "yes" : "no",
        YesNo(gNativeMidpoint.adapterObserved),
        YesNo(gNativeMidpoint.fallbackActive));
    ImGui::Text("Midpoint backport: enabled %s | applied %s | at Create %s",
        YesNo(gNativeMidpoint.backportEnabled),
        YesNo(gNativeMidpoint.backportApplied),
        YesNo(gNativeMidpoint.readyAtCreate));
    ImGui::Text("Midpoint outputs: raw %u | unique %u",
        gNativeMidpoint.rawCount,
        gNativeMidpoint.uniqueOutputs);
#endif

    ImGui::Separator();
    ImGui::TextUnformatted("Runtime");
    if (snapshot.getStateSeen && snapshot.lastGetStateResult == 0)
    {
        ImGui::Text("NGX last sample: %u frames since prior state query",
            snapshot.actualFramesPresented);
        ImGui::Text("Capability: up to %ux (%u generated per real frame)",
            snapshot.numFramesToGenerateMax + 1,
            snapshot.numFramesToGenerateMax);
    }
    else
    {
        ImGui::TextUnformatted("NGX reported: waiting for active FG state");
    }
    if (snapshot.setOptionsSeen)
    {
        ImGui::Text("SetOptions: %d (%s)",
            snapshot.lastSetOptionsResult,
            StreamlineResultLabel(snapshot.lastSetOptionsResult));
    }
    if (snapshot.getStateSeen)
    {
        ImGui::Text("GetState: %d (%s)",
            snapshot.lastGetStateResult,
            StreamlineResultLabel(snapshot.lastGetStateResult));
    }

    const char* uiState = snapshot.uiRecompositionForced
        ? "legacy override accepted" : snapshot.uiRecompositionEnabled
            ? "game option enabled (last accepted)" : "game-managed";
    ImGui::Text("UI recomposition: %s", uiState);
    ImGui::Text("Tagged UI input coherence: %s",
        snapshot.uiInputsReady ? "established" : "not established");
    ImGui::TextUnformatted("Tag observations do not verify displayed UI.");
    DrawHudlessDetection(snapshot);

    ImGui::Separator();
    ImGui::TextUnformatted("Streamline and OTA");
    ImGui::Text("Host: %u.%u.%u.%u | slInit calls: %llu",
        snapshot.streamlineHostVersionMajor,
        snapshot.streamlineHostVersionMinor,
        snapshot.streamlineHostVersionBuild,
        snapshot.streamlineHostVersionPrivate,
        static_cast<unsigned long long>(snapshot.slInitCalls));
    ImGui::Text("slInit entry: %s | IAT fallback: %s | resolver: %s",
        YesNo(snapshot.slInitEntryDetourCurrent),
        YesNo(snapshot.slInitIatFallbackInstalled),
        YesNo(snapshot.slInitResolverFallbackActive));
    ImGui::Text("slInit control: %s | flags: 0x%llX -> 0x%llX",
        YesNo(snapshot.slInitControlPathReady),
        static_cast<unsigned long long>(snapshot.slInitFlagsBefore),
        static_cast<unsigned long long>(snapshot.slInitFlagsAfter));
    ImGui::Text("Full OTA requested/eligible: %s/%s | plugins forced: %s",
        YesNo(snapshot.fullStreamlineOtaRequested),
        YesNo(snapshot.fullStreamlineOtaEligible),
        YesNo(snapshot.downloadedStreamlinePluginsForced));
    ImGui::Text("OTA preference forced/at init: %s/%s",
        YesNo(snapshot.otaPreferencesForced),
        YesNo(snapshot.otaPreferencesEnabledAtInit));
    ImGui::Text("Downloaded plugins at init: %s | preflight: %s | suppressed: %s",
        YesNo(snapshot.downloadedStreamlinePluginsEnabledAtInit),
        YesNo(snapshot.otaProviderPreflightSupported),
        YesNo(snapshot.otaForceSuppressed));
#if !defined(MFG_UNLOCK_V12_UNIVERSAL_UI)
    ImGui::Text("Legacy selective OTA requested/candidate: %s/%s | failure: %u",
        YesNo(snapshot.selectiveOtaDlssgWrapperRequested),
        YesNo(snapshot.selectiveOtaDlssgWrapperCandidateReady),
        snapshot.selectiveOtaDlssgWrapperFailure);
    ImGui::Text("Legacy redirects: %llu attempts | %llu successes | %llu fallbacks",
        static_cast<unsigned long long>(
            snapshot.selectiveOtaDlssgWrapperRedirectAttempts),
        static_cast<unsigned long long>(
            snapshot.selectiveOtaDlssgWrapperRedirectSuccesses),
        static_cast<unsigned long long>(
            snapshot.selectiveOtaDlssgWrapperFallbacks));
    ImGui::Text("Legacy candidate: %u.%u.%u.%u",
        snapshot.selectiveOtaDlssgWrapperVersionMajor,
        snapshot.selectiveOtaDlssgWrapperVersionMinor,
        snapshot.selectiveOtaDlssgWrapperVersionBuild,
        snapshot.selectiveOtaDlssgWrapperVersionPrivate);
#endif

    ImGui::Separator();
    ImGui::TextUnformatted("Interval trace (always on)");
    ImGui::Text("Log ready: %s", YesNo(snapshot.intervalLogReady));
    ImGui::Text("Samples: %llu valid | %llu invalid | %llu dropped",
        static_cast<unsigned long long>(snapshot.intervalValidSamples),
        static_cast<unsigned long long>(snapshot.intervalInvalidSamples),
        static_cast<unsigned long long>(snapshot.intervalDroppedSamples));
    if (snapshot.intervalValidSamples > 0)
    {
        ImGui::Text("Last: count %d | index %d | position %u/%u",
            snapshot.intervalLastCount,
            snapshot.intervalLastIndex,
            snapshot.intervalLastPositionNumerator,
            snapshot.intervalLastPositionDenominator);
    }
    if (!snapshot.ngxEvaluateEntryDetourCurrent)
    {
        ImGui::TextUnformatted("Status: Evaluate hook unavailable");
    }
    else if (snapshot.intervalValidSamples == 0)
    {
        ImGui::TextUnformatted(snapshot.intervalLogReady
            ? "Status: waiting for FG Evaluate"
            : "Status: opening log");
    }
    if (snapshot.intervalLogFile[0])
    {
        ImGui::Text("Trace file: %%TEMP%%\\%s",
            snapshot.intervalLogFile);
    }
    }
    ImGui::EndDisabled();
#endif

    if (settingsChanged)
        ApplySettings(runtime, true);
#if !defined(MFG_UNLOCK_SINGLE_MODULE_UI)
    if (nativeFile)
        ImGui::TextDisabled("Status: %s", statusStale
            ? "Temporarily unavailable; controls paused" : "Live");
#endif
}
}

#if !defined(MFG_UNLOCK_SINGLE_MODULE_UI)
extern "C" __declspec(dllexport) const char* AUTHOR = "dashdogy";

#if defined(MFG_UNLOCK_RESHADE_UI_CLIENT)
extern "C" __declspec(dllexport) const char* NAME =
    "Universal RTX 40 MFG Unlock V1.3";
extern "C" __declspec(dllexport) const char* DESCRIPTION =
    "Universal DLSS Multi Frame Generation enabler for supported games on "
    "NVIDIA GeForce RTX 40 Series GPUs.";
#else
extern "C" __declspec(dllexport) const char* NAME =
    "DLSS MFG Unlock - ReShade Early Load";
extern "C" __declspec(dllexport) const char* DESCRIPTION =
    "Universal DLSS Multi Frame Generation enabler for supported games on "
    "NVIDIA GeForce RTX 40 Series GPUs.";
#endif

extern "C" __declspec(dllexport) bool AddonInit(
    HMODULE addonModule, HMODULE reshadeModule)
{
    if (addonModule != gSelf || reshadeModule != gReShade
        || !gRegistered.load(std::memory_order_acquire))
    {
        return false;
    }
    bool expected = false;
    if (!gInitialized.compare_exchange_strong(expected, true,
            std::memory_order_acq_rel, std::memory_order_acquire))
    {
        return true;
    }
    gOverlayVisitedThisFrame = false;
    gOverlayDockedThisFrame = false;
    RegisterRuntimeEvents();
    LoadSettings();
    reshade::register_overlay(kOverlayTitle, DrawSettings);
    return true;
}

extern "C" __declspec(dllexport) void AddonUninit(
    HMODULE addonModule, HMODULE reshadeModule)
{
    if (addonModule != gSelf || reshadeModule != gReShade)
        return;
    if (gInitialized.exchange(false, std::memory_order_acq_rel))
    {
        reshade::unregister_overlay(kOverlayTitle, DrawSettings);
        UnregisterRuntimeEvents();
    }
    if (gRegistered.exchange(false, std::memory_order_acq_rel))
        reshade::unregister_addon(addonModule, reshadeModule);
#if defined(MFG_UNLOCK_RESHADE_UI_CLIENT)
    DisconnectBackend();
#endif
}

namespace reshade_frontend
{
bool ProcessAttach(HMODULE self) noexcept
{
#if defined(MFG_UNLOCK_RESHADE_UI_CLIENT)
    // Backend discovery is intentionally non-fatal. ReShade may initialize
    // before a misconfigured or delayed ASI transport; keeping the UI loaded
    // preserves a visible diagnostic surface and permits a later connection.
    if (!self)
        return false;
    if (!reshade::register_addon(self))
    {
        return false;
    }
    HMODULE reshadeModule = reshade::internal::get_reshade_module_handle();
    if (!reshadeModule)
    {
        reshade::unregister_addon(self, reshadeModule);
        return false;
    }
#else
    if (!self || ExistingMfgBackendPresent(self))
        return false;
    if (!reshade::register_addon(self))
        return false;

    HMODULE reshadeModule = reshade::internal::get_reshade_module_handle();
    if (!reshadeModule
        || !central_feature_hook::ConfigureReShadeEarlyLoad(reshadeModule))
    {
        reshade::unregister_addon(self, reshadeModule);
        return false;
    }
#endif
    gSelf = self;
    gReShade = reshadeModule;
    gRegistered.store(true, std::memory_order_release);
    return true;
}

void ProcessDetach(HMODULE self) noexcept
{
    if (self != gSelf)
        return;
    if (gInitialized.exchange(false, std::memory_order_acq_rel))
    {
        reshade::unregister_overlay(kOverlayTitle, DrawSettings);
        UnregisterRuntimeEvents();
    }
    if (gRegistered.exchange(false, std::memory_order_acq_rel))
        reshade::unregister_addon(self, gReShade);
#if defined(MFG_UNLOCK_RESHADE_UI_CLIENT)
    DisconnectBackend();
#endif
}
}

#else
namespace standalone_ui
{
void Initialize(HMODULE self)
{
    gSelf = self;
    if (!gInitialized.exchange(true, std::memory_order_acq_rel))
        LoadSettings();
}
void Draw()
{
    DrawSettings(nullptr);
}
void DrawFooterStatus()
{
    if (!gFooterStatus[0]) return;
    const float width = ImGui::CalcTextSize(gFooterStatus).x;
    ImGui::SameLine();
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(0.0f, ImGui::GetContentRegionAvail().x - width));
    ImGui::AlignTextToFramePadding();
    ImGui::TextColored(ToneColor(gFooterTone), "%s", gFooterStatus);
    if (std::strcmp(gFooterStatus, "Paused") == 0 && ImGui::IsItemHovered())
        ImGui::SetTooltip("Status is temporarily unavailable; controls are paused.");
}
}
#endif
