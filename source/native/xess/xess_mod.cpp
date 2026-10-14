#include "xess_mod.h"
#include "runtime_payload.h"
#include "game_profile.h"
#include "frame_time_profile.h"
#include "sdk_pacing.h"
#include "xess_api.h"
#include "xess_policy.h"
#include "adapter_support.h"
#include "xess_runtime.h"
#include "present_capture.h"
#include "single_overlay.h"
#include "single_module.h"
#include "overlay_device_identity.h"
#include "overlay_dx12.h"
#include "present_counter.h"
#include "port/port_support.h"
#include "port/model_port.h"
#include "port/shader_port.h"
#include "host_unlock.h"
#include "motion_fill.h"
#include "hudless_match.h"
#include "cadence_stats.h"
#include "optiscaler_host.h"
#include <atomic>
#include <algorithm>
#include <cstring>
#include <map>
#include <mutex>
#include <memory>
#include <set>
#include <utility>
#include <cmath>
#include <limits>
#include <optional>

namespace xess_mod
{
namespace
{
constexpr auto kInvalid = XEFG_SWAPCHAIN_RESULT_ERROR_INVALID_CONTEXT;
constexpr auto kArgument = XEFG_SWAPCHAIN_RESULT_ERROR_INVALID_ARGUMENT;

struct Callback
{
    xefg_swapchain_app_log_callback_t function{};
    void* user{};
};
struct FgContext
{
    xefg_swapchain_handle_t handle{};
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12CommandQueue> presentationQueue;
    ComPtr<IDXGISwapChain> chain;
    ComPtr<IDXGISwapChain> uiProxy;
    xell_context_handle_t latency{};
    // beta.8 923: paired with a XeLL context OptiScaler's XeLL input owns (untracked, no reference held).
    bool hostLatency{};
    uint64_t generation{};
    // The caller's interpolation count (the game's, or OptiScaler's in its pass-through) is applied
    // at the next frame boundary while pending.
    bool countPending{true};
    uint32_t applied{};
    uint32_t capacity{};
    uint32_t gameGenerated{1};
    bool gameComposition{true};
    bool compositionManagedByGame{};
    xefg_swapchain_properties_t resizeProperties{};
    bool resizePropertiesKnown{};
    uint32_t width{}, height{}, uiMode{};
    bool initialized{}, enabled{}, closing{}, poisoned{}, composition{};
    bool gameEnabled{}, presentIdKnown{};
    uint32_t presentId{};
    bool uiAttached{}, initializationAttempted{};
    int32_t initializationFailure{};
    int32_t destructionFailure{};
    bool presentStatusAvailable{};
    xefg_swapchain_present_status_t presentStatus{};
    uint64_t presentStatusTick{}, presentStatusLogTick{};
    std::array<uint64_t, 5> resourceAttempts{}, resourceAccepted{};
    std::array<int32_t, 5> resourceResults{};
    uint64_t invalidResourceCalls{};
    uint64_t constantsAttempts{};
    // Game frame cadence: the frame time the game reports and the time between its tag calls.
    cadence_stats::Window<120> gameFrameMs, gameTagGapMs;
    int64_t lastConstantsQpc{};
    // dev.33: frameRenderTime units of this game (no exe profile needed).
    frame_time_profile::UnitDetector units;
    bool unitsLogged{};
    uint32_t initFlags{}, constantsFrame{};
    bool constantsAvailable{};
    xefg_swapchain_frame_constant_data_t frameConstants{};
    std::mutex providerLogMutex;
    std::string lastProviderLog;
    uint64_t providerLogTick{}, repeatedProviderLogs{};
    ColorTags tags;
    // dev.43: the (2,2) motion-vector marker filled with camera motion (tracked contexts only).
    std::unique_ptr<xess_motion_fill::Filler> motionFill;
    std::set<std::string> motionFillReasons; // each skip/failure reason is logged once
    bool motionFillLogged{};
    // dev.47: the game's HUD-less input converted to the back buffer's encoding (portable contexts).
    std::unique_ptr<xess_hudless_match::Matcher> hudlessMatch;
    std::set<std::string> hudlessMatchReasons;
    bool hudlessMatchLogged{}, hudlessMatchedKnown{};
    uint32_t hudlessMatchedFrame{};
    std::atomic<std::shared_ptr<const Callback>> callback;
};
struct LlContext
{
    xell_context_handle_t handle{};
    uint32_t references{};
    ComPtr<ID3D12Device> device;
    uint32_t activeCalls{};
};
struct State
{
    std::once_flag configurationOnce, loadOnce;
    std::recursive_mutex fgMutex;
    std::recursive_mutex llMutex;
    std::mutex logMutex;
    std::filesystem::path root;
    xess_runtime::Source source{};
    // Untracked XeSS-FG contexts (Intel GPUs, unprofiled pairs, OptiScaler's own) still tell
    // the menu the game uses XeSS-FG.
    std::atomic<bool> nativeEnabled{};
    std::atomic<uint64_t> nativeActivityTick{};
    std::atomic<std::shared_ptr<const Snapshot>> published{std::make_shared<Snapshot>()};
    Snapshot display;
    Api api;
    HMODULE fgModule{}, llModule{};
    std::atomic<bool> loaded{};
    std::string loadFailure;
    std::map<xefg_swapchain_handle_t, std::unique_ptr<FgContext>> contexts;
    std::map<xell_context_handle_t, LlContext> latency;
    std::set<xefg_swapchain_handle_t> retiredFg;
    std::set<xell_context_handle_t> retiredLl;
    std::unique_ptr<ShaderPort> shaders;
    std::unique_ptr<ModelPort> model;
    // dev.40: contexts OptiScaler's own code created (untracked; fgMutex / llMutex). dev.41:
    // each XeFG one keeps its device and generation for its plain pacing owner (HostInitFg).
    struct HostContext { ComPtr<ID3D12Device> device; uint64_t generation{}; bool paced{}; };
    std::map<xefg_swapchain_handle_t, HostContext> hostFg;
    std::set<xell_context_handle_t> hostLl;
    std::once_flag hostUnlockOnce;
    // dev.40/42: the frame-count unlock (fgMutex), applied once per process to the provider
    // image it names and kept for the process lifetime. Host contexts (dev.40) and, on the
    // 2x-model route, tracked contexts (dev.42) on that image share it.
    std::unique_ptr<xess_host_unlock::Unlock> unlock;
    HMODULE unlockFg{};
    bool hostUnlocked{}, trackedUnlocked{};
    // dev.46: the ported model failed to publish; the 2x model serves the rest of the process.
    bool portRejected{};
    // dev.48: the ported model was published for OptiScaler's own XeFG (kept for the process lifetime).
    bool hostPorted{};
    uint64_t generation{};
    uint64_t foreignResourceCalls{};
    bool uncertainFgLifetime{}, uncertainLlLifetime{};
    HANDLE log = INVALID_HANDLE_VALUE;
    uint64_t logBytes{};
};
State& Get()
{
    // SDK contexts, hooks and callbacks own process lifetime. Explicit successful
    // last-context destruction restores model metadata and the decoder hook.
    // No C++ static destructor can free them while a provider worker is alive.
    static auto* state = new State;
    return *state;
}

thread_local bool tHostCall{}; // dev.40, set by HostCallScope

// Never keep a global lock held while XeLL sleeps. A queued exclusive destroy
// must not prevent another thread from publishing the marker that wakes sleep.
struct LlLease
{
    State& state;
    LlContext* context{};
    bool retired{};
    LlLease(State& owner, xell_context_handle_t handle) : state(owner)
    {
        std::lock_guard lock(state.llMutex);
        retired = state.retiredLl.contains(handle);
        const auto it = state.latency.find(handle);
        if (it != state.latency.end()) { context = &it->second; ++context->activeCalls; }
    }
    ~LlLease()
    {
        if (context) { std::lock_guard lock(state.llMutex); --context->activeCalls; }
    }
    LlLease(const LlLease&) = delete;
};

std::filesystem::path ModulePath(HMODULE module)
{
    std::wstring text(32768,L'\0');
    const DWORD count = GetModuleFileNameW(module, text.data(), static_cast<DWORD>(text.size()));
    if (!count || count >= text.size()) throw std::runtime_error("module_path_unavailable");
    text.resize(count);
    return std::filesystem::path(text);
}

void Publish(State& state, const FgContext* context = nullptr)
{
    auto result = state.display;
    result.importsInstalled = Active();
    result.providerLoaded = state.loaded;
    result.portReady = state.model != nullptr || state.trackedUnlocked;
    result.modelActive = state.model ? 1u : state.unlock && state.unlock->Keeps2xModel() ? 2u : 0u;
    result.runtimeSource = static_cast<uint32_t>(state.source);
    result.runtimeFailure = state.loadFailure;
    result.nativeEnabled = state.nativeEnabled.load(std::memory_order_acquire);
    result.nativeActivityTick = state.nativeActivityTick.load(std::memory_order_acquire);
    if (context)
    {
        result.initialized = context->initialized && !context->closing && !context->poisoned;
        result.enabled = context->enabled;
        result.gameEnabled = context->gameEnabled;
        result.applied = context->applied;
        result.capacity = context->capacity;
        result.generation = context->generation;
        result.composition = context->composition;
        result.compositionManagedByGame = context->compositionManagedByGame;
        result.compositionRequested = context->gameComposition;
        result.taggedResources = context->tags.mask;
        result.taggedFrameId = context->tags.frame;
        result.resourceAttempts = context->resourceAttempts;
        result.resourceAccepted = context->resourceAccepted;
        result.resourceResults = context->resourceResults;
        result.presentStatusAvailable = context->presentStatusAvailable;
        result.providerEnabled = context->presentStatus.isFrameGenEnabled != 0;
        result.lastFrameGenResult = context->presentStatus.frameGenResult;
        result.framesPresented = context->presentStatus.framesPresented;
        result.lastStateTick = context->presentStatusTick;
        if (context->destructionFailure < 0)
        {
            result.lastResult = context->destructionFailure;
            result.status = "XeSS destruction failed ("+std::to_string(context->destructionFailure)+"). Restart required.";
        }
        else if (context->initializationFailure < 0)
        {
            result.lastResult = context->initializationFailure;
            result.status = "XeSS initialization failed ("+std::to_string(context->initializationFailure)+"). Restart required.";
        }
        else if (result.initialized && context->enabled && (result.lastResult >= 0
            || (context->presentStatusAvailable && context->presentStatus.frameGenResult < 0)))
        {
            if (!context->presentStatusAvailable)
                result.status = "XeSS initialized; waiting for presentation status.";
            else if (context->presentStatus.frameGenResult < 0)
                result.status = "XeSS frame generation failed ("+std::to_string(context->presentStatus.frameGenResult)+
                    "). Check the input and provider status below.";
            else if (!context->presentStatus.isFrameGenEnabled)
                result.status = "XeSS is enabled; the provider reports generation inactive.";
            else if (context->presentStatus.framesPresented <= 1)
                result.status = "XeSS is enabled, but the provider reports no generated frames.";
            else
                result.status = "XeSS reports additional frames. Generated content is not yet verified.";
        }
    }
    state.published.store(std::make_shared<Snapshot>(std::move(result)), std::memory_order_release);
}

void Failure(State& state, const char* reason, int32_t result = XEFG_SWAPCHAIN_RESULT_ERROR_UNKNOWN)
{
    state.display.status = reason;
    state.display.lastResult = result;
    WriteLog("failure", "\"reason\":\""+escape(reason)+"\",\"result\":"+std::to_string(result));
    Publish(state);
}

void Configuration(State& state)
{
    std::call_once(state.configurationOnce, [&] {
        // beta.8: OptiScaler's menu owns the frame count; the model choice comes through ConfigureModel.
        state.root = xess_runtime::ModulePath(Self()).parent_path();
    });
}

bool GameCopyMatches(HMODULE fg, HMODULE ll)
{
    if (!fg || !ll) return false;
    const auto fgHash = sha256(ModulePath(fg));
    const auto llHash = sha256(ModulePath(ll));
    WriteLog("runtime_game_copy", "\"fg_sha256\":\""+fgHash+"\",\"ll_sha256\":\""+llHash+"\"");
    return xess_layout::Pair(fg, ll);
}

// The runtime is the discovered pair (under OptiScaler, the copy its XeFG output uses) when its
// instruction layout is supported. Otherwise Intel's provider stays in use as the game loaded it
// (one generated frame on non-Intel GPUs).
bool EnsureRuntime(State& state)
{
    Configuration(state);
    std::call_once(state.loadOnce, [&] {
        try
        {
            if (!Active()) throw std::runtime_error("sdk_route_not_ready");
            // Validate the embedded shader set before any provider is ported.
            xess_payload::LoadPrograms(Self());
            std::string routeFailure;
            const HMODULE gameFg = NativeFgModule(), gameLl = NativeLlModule();
            if (GameCopyMatches(gameFg, gameLl))
            {
                // The game already loaded a compatible pair. Its own image is
                // ported in memory and our calls use the hook trampolines.
                state.fgModule = gameFg;
                state.llModule = gameLl;
                state.source = xess_runtime::Source::eGameCopy;
                if (!sdk_pacing::Install(state.fgModule, routeFailure))
                    throw std::runtime_error(routeFailure.empty() ? "game_pacing_route_rejected" : routeFailure);
                state.api = original;
            }
            else throw std::runtime_error("game_pair_not_profiled");
            state.loaded = true;
            WriteLog("runtime_ready", "\"source\":\""+std::string(xess_runtime::SourceName(state.source))+
                "\",\"profile\":\""+xess_layout::Get(state.fgModule).family+"\",\"programs\":54");
        }
        catch (const std::exception& error)
        {
            state.source = xess_runtime::Source::eNone;
            state.loadFailure = error.what();
            WriteLog("runtime_rejected", "\"reason\":\""+escape(state.loadFailure)+"\"");
        }
    });
    return state.loaded;
}

std::string VerifyAdapter(ID3D12Device* device)
{
    const auto adapter = adapter_support::Inspect(device);
    const auto& description = adapter.description;
    WriteLog("adapter", "\"name\":\""+escape(utf8(description.Description))+"\",\"vendor\":"+
        std::to_string(description.VendorId)+",\"device\":"+std::to_string(description.DeviceId)+
        ",\"route\":\""+adapter_support::RouteName(adapter.route)+
        "\",\"shader_model_minimum\":100,\"luid_high\":"+std::to_string(description.AdapterLuid.HighPart)+
        ",\"luid_low\":"+std::to_string(description.AdapterLuid.LowPart)+
        ",\"driver_version_raw\":"+std::to_string(adapter.driver.QuadPart));
    return utf8(description.Description);
}

bool SameDevice(ID3D12Device* a, ID3D12Device* b)
{
    ComPtr<IUnknown> first, second;
    return single_overlay::dx12::DeviceIdentity(a, first) && single_overlay::dx12::DeviceIdentity(b, second)
        && first.Get() == second.Get();
}

// dev.46: the model that generates 3x-6x for the game's own XeFG contexts on non-Intel GPUs (beta.8:
// OptiScaler's pass-through of the game's XeFG input). Default: our port of Intel's MFG model
// (ModelPort 15 -> 16; XeFG remaps its 2x model to it for more than one generated frame), with the
// plain pacing owner. XeFG's 2x model (dev.42-45, kept by the fg_keep_2x_model site) rendered the
// six-times outputs at t ~0.25/0.33/0.5/0.65/0.75 instead of 1/6..5/6 (dev.45 capture): smeared outer
// outputs. RTXMFG_XESS_MODEL=2x, or xefg_model=2x in RTXMFG-capture.enable, selects the 2x model;
// otherwise OptiScaler.ini [XeFG] XeFGModel (xefgmodel.1; earlier RtxmfgModel), then the control
// file's xessModel. Read once, at the first tracked creation.
std::atomic<uint32_t> gConfiguredModel{0};
std::atomic<uint32_t> gHostModel{0};

// beta.8 modelchoice.1: one decision per route, made at its first creation, with OptiScaler.ini read
// at that moment. The worker's earlier read only seeds gHostModel: a creation before the worker read
// it fell back to auto (port) for the whole session, so "intel" could be ignored. The source, the raw
// setting and (xefgmodel.1) the ini key go into the route's log line.
struct ModelChoice
{
    bool port = true;
    const char* source = "default";
    std::string setting, ini, key;
};

ModelChoice ResolveModelChoice(const char* environment, const char* markerKey) noexcept
{
    ModelChoice choice;
    try
    {
        char value[8]{};
        const DWORD length = GetEnvironmentVariableA(environment, value, DWORD(sizeof(value)));
        if (length && length < sizeof(value))
        {
            choice.source = "environment";
            choice.setting.assign(value, length);
            choice.port = _stricmp(choice.setting.c_str(), "2x") != 0;
            return choice;
        }
        if (auto option = xess_capture::StartupOption(markerKey); !option.empty())
        {
            choice.source = "capture_marker";
            choice.setting = option;
            choice.port = _stricmp(option.c_str(), "2x") != 0;
            return choice;
        }
        std::wstring setting, ini, key;
        uint32_t host = optiscaler_host::ReadXefgModel(setting, ini, key);
        choice.setting = utf8(setting);
        choice.ini = utf8(ini);
        choice.key = utf8(key);
        if (host != 1 && host != 2 && ini.empty()) host = gHostModel.load(std::memory_order_acquire);
        if (host == 1 || host == 2)
        {
            choice.source = "optiscaler_ini";
            choice.port = host == 1;
            return choice;
        }
        choice.source = "control";
        choice.port = gConfiguredModel.load(std::memory_order_acquire) != 1;
    }
    catch (...) { choice = ModelChoice{}; }
    return choice;
}

std::string ModelChoiceFields(const ModelChoice& choice)
{
    return ",\"model_source\":\""+std::string(choice.source)+"\",\"model_setting\":\""+escape(choice.setting)+
        "\",\"model_key\":\""+escape(choice.key)+"\",\"model_ini\":\""+escape(choice.ini)+"\"";
}

const ModelChoice& TrackedModelChoice() noexcept
{
    static const ModelChoice choice = ResolveModelChoice("RTXMFG_XESS_MODEL", "xefg_model");
    return choice;
}

bool PortedModelRequested() noexcept
{
    return TrackedModelChoice().port;
}

// OptiScaler's own XeFG (host contexts) on our port of Intel's MFG model instead of XeFG's 2x model.
// dev.48 added it as a test option; beta.1 makes it the default: the dev.49 Witcher capture through
// OptiScaler showed interpolated 6x outputs (no copies) at ~0.71-0.76 of a real frame's sharpness, and
// the 2x model renders the outer outputs at the wrong timesteps (dev.45). RTXMFG_XESS_OPTISCALER_MODEL=2x,
// or optiscaler_model=2x in RTXMFG-capture.enable beside this module, selects XeFG's 2x model (dev.41);
// beta.5: otherwise OptiScaler.ini [XeFG] XeFGModel (xefgmodel.1; earlier RtxmfgModel), read when the
// model is chosen (modelchoice.1), then the control file's xessModel. Decided once, at OptiScaler's
// first XeFG creation.
const ModelChoice& HostModelChoice() noexcept
{
    static const ModelChoice choice = ResolveModelChoice("RTXMFG_XESS_OPTISCALER_MODEL", "optiscaler_model");
    return choice;
}

bool HostPortRequested() noexcept
{
    return HostModelChoice().port;
}

// The 16 reconstruction program pairs the ported model needs from the payload.
constexpr const char* kReconstructionPrograms[] = {"3518243144275597942266", "3518243145375109570477",
    "3519166589661394643131", "3519166588561883014920", "3515873413746975655382", "3515873414846487283593",
    "35115408591383412559519", "35115408590283900931308", "35118300971948070613363", "35118300970848558985152",
    "351747859676298538780", "351747860775810166991", "3517111067725881270903", "3517111066626369642692",
    "35110677435103722760616", "35110677436203234388827"};

// Under fgMutex. Applies the frame-count unlock on first use; later callers must name the
// same provider image and the same 2x-model choice. Returns the applied site names as a JSON
// string list body.
std::string AcquireUnlock(State& state, HMODULE fg, HMODULE ll, bool keep2xModel)
{
    if (!state.unlock)
    {
        state.unlock = std::make_unique<xess_host_unlock::Unlock>(fg, ll, static_cast<uint8_t>(kGeneratedCapacity),
            keep2xModel);
        state.unlockFg = fg;
    }
    else if (state.unlockFg != fg) throw std::runtime_error("unlock_on_other_provider_image");
    else if (state.unlock->Keeps2xModel() != keep2xModel) throw std::runtime_error("unlock_model_site_mismatch");
    std::string names;
    for (const auto& site : state.unlock->Applied()) names += std::string(names.empty() ? "\"" : ",\"")+site.name+"\"";
    return names;
}

// beta.7-xell.1: the XeLL layout the unlock met (ll10/ll5: gate written; llu*: XeLL 1.2.x, nothing to write).
std::string UnlockXellField(HMODULE ll)
{
    return ",\"xell_layout\":\""+std::string(xess_layout::Get(ll, false).family)+"\"";
}

std::string_view EnvironmentValue(const char* name, char (&value)[16]) noexcept
{
    const DWORD length = GetEnvironmentVariableA(name, value, DWORD(sizeof(value)));
    return length && length < sizeof(value) ? std::string_view(value, length) : std::string_view{};
}

// RTXMFG_XESS_FRAME_TIME_UNITS=ms|s forces the frameRenderTime units (read once).
frame_time_profile::Units ForcedFrameTimeUnits() noexcept
{
    static const auto forced = [] {
        char value[16]{};
        return frame_time_profile::ForcedUnits(EnvironmentValue("RTXMFG_XESS_FRAME_TIME_UNITS", value));
    }();
    return forced;
}

// Every 120 game frames: the frame time the game hands XeSS-FG (milliseconds,
// after unit correction) and the spacing of its tag calls. Uneven values mean
// uneven content timing, which no presentation pacing can hide.
void RecordGameCadence(FgContext& context, double frameMs, uint32_t frame) noexcept
{
    LARGE_INTEGER now{}, frequency{};
    QueryPerformanceCounter(&now); QueryPerformanceFrequency(&frequency);
    if (context.lastConstantsQpc && now.QuadPart > context.lastConstantsQpc && frequency.QuadPart > 0)
        context.gameTagGapMs.Add(double(now.QuadPart-context.lastConstantsQpc)*1000.0/double(frequency.QuadPart));
    context.lastConstantsQpc = now.QuadPart;
    if (!context.gameFrameMs.Add(frameMs)) return;
    try
    {
        WriteLog("game_frame_stats", "\"generation\":"+std::to_string(context.generation)+
            ",\"frame\":"+std::to_string(frame)+",\"multiplier\":"+std::to_string(context.applied)+
            ","+cadence_stats::Json("frame_time_ms", context.gameFrameMs.Take())+
            ","+cadence_stats::Json("tag_gap_ms", context.gameTagGapMs.Take()));
    }
    catch (...) { context.gameFrameMs.Take(); context.gameTagGapMs.Take(); }
}

// dev.42: XeFG's 2x model for every count (the unlock keeps it), before the first tracked creation
// (the override clamp acts at creation).
void PrepareTwoTimesModel(State& state)
{
    if (state.trackedUnlocked) return;
    const auto names = AcquireUnlock(state, state.fgModule, state.llModule, true);
    state.trackedUnlocked = true;
    WriteLog("model_route", "\"route\":\"xefg_2x_model\",\"applied\":["+names+"],\"generated\":"+
        std::to_string(kGeneratedCapacity)+",\"pacing\":\"plain\""+UnlockXellField(state.llModule)+
        ModelChoiceFields(TrackedModelChoice()));
}

void PreparePort(State& state)
{
    if (!PortedModelRequested() || state.portRejected) { PrepareTwoTimesModel(state); return; }
    if (state.model) return;
    if (!state.contexts.empty()) throw std::runtime_error("context_predates_model_publication");
    try
    {
        auto shaders = std::make_unique<ShaderPort>(state.fgModule, Self(), state.root);
        for (const char* key : kReconstructionPrograms)
            if (!shaders->hasProgram(key)) throw std::runtime_error("reconstruction_program_pair_missing");
        auto model = std::make_unique<ModelPort>(state.fgModule, true, true);
        // dev.46: the frame-count unlock without the 2x-model site (kept for the process lifetime).
        if (!state.trackedUnlocked)
        {
            const auto names = AcquireUnlock(state, state.fgModule, state.llModule, false);
            state.trackedUnlocked = true;
            WriteLog("model_route", "\"route\":\"ported_mfg_model\",\"applied\":["+names+"],\"generated\":"+
                std::to_string(kGeneratedCapacity)+",\"pacing\":\"plain\""+UnlockXellField(state.llModule)+
                ModelChoiceFields(TrackedModelChoice()));
        }
        state.shaders = std::move(shaders);
        state.model = std::move(model);
    }
    catch (const std::exception& error)
    {
        // dev.46: the 2x model route is verified separately; the port is not retried.
        state.portRejected = true;
        WriteLog("model_port_rejected", "\"reason\":\""+escape(error.what())+"\",\"fallback\":\"xefg_2x_model\"");
        PrepareTwoTimesModel(state);
    }
}

void RestoreIdlePort(State& state)
{
    // dev.48: a port published for OptiScaler's own XeFG stays (its contexts are untracked).
    // modelchoice.1: so does any port while an OptiScaler context lives on the image.
    if (!state.contexts.empty() || state.uncertainFgLifetime || state.hostPorted || !state.hostFg.empty()) return;
    state.model.reset();
    state.shaders.reset();
}

void FinishFailedCreate(State& state, xefg_swapchain_handle_t created, xefg_swapchain_result_t result)
{
    if (created && !state.contexts.contains(created))
        state.uncertainFgLifetime = state.api.xefgSwapChainDestroy(created) != XEFG_SWAPCHAIN_RESULT_SUCCESS;
    else if (!created && result != XEFG_SWAPCHAIN_RESULT_SUCCESS)
        state.uncertainFgLifetime = false;
    RestoreIdlePort(state);
}

FgContext* FindFg(State& state, xefg_swapchain_handle_t handle)
{
    const auto it = state.contexts.find(handle);
    return it == state.contexts.end() ? nullptr : it->second.get();
}

void ResetPacing(const FgContext& context) noexcept
{ sdk_pacing::Reset(context.handle, context.generation); }
Api& LatencyApi(State& state, const LlLease& lease)
{ return lease.context ? state.api : original; }

void FgLog(const char* message, xefg_swapchain_logging_level_t level, void* user);

void Report(State& state, FgContext& context, xefg_swapchain_result_t result, const char* stage)
{
    state.display.lastResult = result;
    if (result < 0)
    {
        state.display.status = std::string("XeSS error at ")+stage+" ("+std::to_string(result)+").";
        WriteLog("api_error", "\"stage_name\":\""+std::string(stage)+"\",\"result\":"+std::to_string(result));
    }
    Publish(state, &context);
}

void RefreshExtent(FgContext& context)
{
    if (!context.chain) return;
    DXGI_SWAP_CHAIN_DESC description{};
    if (context.chain->GetDesc(&description) == S_OK)
    {
        if (context.width != description.BufferDesc.Width || context.height != description.BufferDesc.Height)
            ResetPacing(context);
        context.width = description.BufferDesc.Width;
        context.height = description.BufferDesc.Height;
    }
}

// dev.44: the UI composition override (xess_mod.h InputDiagnostics::uiCompositionOff).
std::atomic<bool> uiCompositionOff{::xess_mod::UiCompositionOffDefault()};
// dev.47: the HUD-less matcher (xess_mod.h InputDiagnostics::hudless).
std::atomic<uint32_t> hudlessMode{::xess_mod::HudlessMatchDefault()};
bool HudlessMatching() noexcept
{
    return !uiCompositionOff.load(std::memory_order_acquire)
        && hudlessMode.load(std::memory_order_acquire) == uint32_t(xess_hudless_match::Mode::eMatch);
}

xefg_swapchain_result_t UpdateComposition(State& state, FgContext& context, uint32_t frame)
{
    if (!context.initialized || context.closing || context.poisoned) return XEFG_SWAPCHAIN_RESULT_ERROR_WRONG_CALL_ORDER;
    RefreshExtent(context);
    // dev.44: Witcher (ui_mode 4 with a 4K HUDless tag) is composed by default; with composition
    // on, every generated output was a byte-exact copy of the nearest real frame (dev.38-43
    // captures), whatever its motion vectors. The override keeps it off for comparison.
    const bool off = uiCompositionOff.load(std::memory_order_acquire);
    // dev.47: with the matcher on, only a frame whose HUD-less input reached XeFG in the back buffer's
    // encoding is composed (Witcher's own HUD-less input is linear pre-tonemap, the back buffer HDR10:
    // XeFG extracts the whole screen as UI and presents copies).
    const bool hudlessReady = !HudlessMatching() || (context.hudlessMatchedKnown && context.hudlessMatchedFrame == frame);
    const bool enable = !off && hudlessReady && context.gameComposition
        && context.tags.CanCompose(frame, context.uiMode, context.width, context.height);
    if (enable == context.composition) return XEFG_SWAPCHAIN_RESULT_SUCCESS;
    const auto result = state.api.xefgSwapChainSetUiCompositionState(context.handle,
        enable ? XEFG_SWAPCHAIN_UI_COMPOSITION_STATE_ENABLED : XEFG_SWAPCHAIN_UI_COMPOSITION_STATE_DISABLED);
    if (result == XEFG_SWAPCHAIN_RESULT_SUCCESS) context.composition = enable;
    else Report(state, context, result, "UI composition");
    // dev.45: the first 16 changes, then every 1000th (dev.44 logged two per frame).
    static std::atomic<uint64_t> changes{};
    const auto change = changes.fetch_add(1, std::memory_order_relaxed)+1;
    if (change <= 16 || change%1000 == 0)
        WriteLog("ui_composition", "\"generation\":"+std::to_string(context.generation)+",\"frame\":"+std::to_string(frame)+
            ",\"enabled\":"+(enable ? "true" : "false")+",\"ui_mode\":"+std::to_string(context.uiMode)+
            ",\"override_off\":"+(off ? "true" : "false")+",\"hudless_ready\":"+(hudlessReady ? "true" : "false")+
            ",\"result\":"+std::to_string(result)+
            ",\"changes\":"+std::to_string(change));
    return result;
}

// beta.8: the caller's interpolation count (the game's, or OptiScaler's in its pass-through), applied
// at its frame boundary under the lock that guards initialization and destruction.
void ApplyPending(State& state, FgContext& context)
{
    if (!context.countPending || context.poisoned) return;
    if (!CanApply(0, context.capacity, context.initialized, FindFg(state, context.handle) == &context, context.closing)) return;
    xefg_swapchain_d3d12_init_params_t current{};
    auto result = state.api.xefgSwapChainD3D12GetInitializationParameters(context.handle, &current);
    if (result != XEFG_SWAPCHAIN_RESULT_SUCCESS || current.maxInterpolatedFrames != kGeneratedCapacity)
    {
        context.poisoned = true;
        context.enabled = false;
        sdk_pacing::Reset(context.handle, context.generation);
        state.api.xefgSwapChainSetEnabled(context.handle, 0);
        Failure(state, "Live XeSS capacity changed; restart the game.");
        Publish(state, &context);
        return;
    }
    const uint32_t generated = context.gameGenerated;
    if (!generated || generated > context.capacity) return;
    result = state.api.xefgSwapChainSetNumInterpolatedFrames(context.handle, generated);
    if (result == XEFG_SWAPCHAIN_RESULT_SUCCESS)
    {
        if (context.applied != generated+1) sdk_pacing::Reset(context.handle, context.generation);
        context.applied = generated+1;
        context.countPending = false;
        state.display.status = "Experimental XeSS MFG ready.";
        WriteLog("multiplier_applied", "\"generation\":"+std::to_string(context.generation)+
            ",\"multiplier\":"+std::to_string(context.applied));
    }
    else
    {
        context.poisoned = true;
        context.enabled = false;
        sdk_pacing::Reset(context.handle, context.generation);
        state.api.xefgSwapChainSetEnabled(context.handle, 0);
    }
    Report(state, context, result, "multiplier change");
}

void FgLog(const char* message, xefg_swapchain_logging_level_t level, void* user)
{
    auto* context = static_cast<FgContext*>(user);
    if (level >= XEFG_SWAPCHAIN_LOGGING_LEVEL_WARNING)
    {
        std::lock_guard lock(context->providerLogMutex);
        const auto tick = GetTickCount64();
        const std::string text = message ? message : "";
        const bool repeated = text == context->lastProviderLog;
        if (repeated) ++context->repeatedProviderLogs;
        if (!repeated || tick - context->providerLogTick >= 2000)
        {
            WriteLog("provider_log", "\"level\":"+std::to_string(level)+",\"message\":\""+escape(text)+
                "\",\"repeats_since_previous_log\":"+std::to_string(repeated ? context->repeatedProviderLogs : 0));
            context->lastProviderLog = text;
            context->providerLogTick = tick;
            context->repeatedProviderLogs = 0;
        }
    }
    const auto callback = context->callback.load(std::memory_order_acquire);
    if (callback && callback->function) callback->function(message, level, callback->user);
}
}

HostCallScope::HostCallScope(bool host) noexcept : previous(tHostCall) { tHostCall = host; }
HostCallScope::~HostCallScope() { tHostCall = previous; }

// A host-created XeLL context also counts through OptiScaler's XeLL wrapper (its real handle,
// with the matching libxell header; see FindLatencyContext).
bool HostOwnedHandle(const void* handle, bool fg) noexcept
{
    if (!handle) return false;
    try
    {
        auto& state = Get();
        if (fg)
        {
            std::lock_guard lock(state.fgMutex);
            return state.hostFg.contains(static_cast<xefg_swapchain_handle_t>(const_cast<void*>(handle)));
        }
        std::lock_guard lock(state.llMutex);
        if (state.hostLl.empty()) return false;
        if (state.hostLl.contains(static_cast<xell_context_handle_t>(const_cast<void*>(handle)))) return true;
        uint64_t wrapper[3]{}, header[2]{};
        SIZE_T got{};
        if (!ReadProcessMemory(GetCurrentProcess(), handle, wrapper, sizeof(wrapper), &got) || got != sizeof(wrapper))
            return false;
        const auto real = reinterpret_cast<xell_context_handle_t>(static_cast<uintptr_t>(wrapper[2]));
        return state.hostLl.contains(real)
            && ReadProcessMemory(GetCurrentProcess(), real, header, sizeof(header), &got) && got == sizeof(header)
            && header[0] == wrapper[0] && header[1] == wrapper[1];
    }
    catch (...) { return false; }
}

void WriteLog(const char* stage, const std::string& fields) noexcept
{
    try
    {
        // One log: XeSS events join RTXMFG's log through the single-module queue.
        const auto line = std::string("XESS {\"stage\":\"")+stage+"\""+(fields.empty() ? "" : ","+fields)+"}";
        const int count = MultiByteToWideChar(CP_UTF8, 0, line.data(), static_cast<int>(line.size()), nullptr, 0);
        if (count <= 0) return;
        std::wstring wide(static_cast<size_t>(count), L'\0');
        MultiByteToWideChar(CP_UTF8, 0, line.data(), static_cast<int>(line.size()), wide.data(), count);
        single_module::Log(wide.c_str());
    }
    catch (...) {}
}

void InitializeOutsideLoaderLock() noexcept
{
    try
    {
        (void)WitcherProfile(); // Cache executable identity before frame callbacks.
        auto& state = Get();
        Configuration(state);
        {
            std::lock_guard lock(state.fgMutex);
            WriteLog("asi_start", "\"route\":\"universal_sdk\",\"asi_sha256\":\""+sha256(ModulePath(Self()))+"\"");
            Publish(state);
        }
        xess_capture::Start(); // dev.37 diagnostic; nothing unless opted in
    }
    catch (...) {}
}

std::shared_ptr<const Snapshot> ReadSnapshot() noexcept
{
    try
    {
        auto& state = Get();
        const auto snapshot = state.published.load(std::memory_order_acquire);
        // Untracked contexts never publish from their per-frame hooks; their
        // activity is read here instead.
        const bool nativeEnabled = state.nativeEnabled.load(std::memory_order_acquire);
        const uint64_t nativeTick = state.nativeActivityTick.load(std::memory_order_acquire);
        if (snapshot->nativeEnabled == nativeEnabled && snapshot->nativeActivityTick == nativeTick) return snapshot;
        auto current = std::make_shared<Snapshot>(*snapshot);
        current->nativeEnabled = nativeEnabled;
        current->nativeActivityTick = nativeTick;
        return current;
    }
    catch (...) { return {}; }
}

bool PrivateApiHandle(bool fg, void* handle) noexcept
{
    try {
        auto& state = Get();
        if (fg) {
            std::lock_guard lock(state.fgMutex);
            const auto typed = static_cast<xefg_swapchain_handle_t>(handle);
            const auto it = state.contexts.find(typed);
            return it != state.contexts.end() || state.retiredFg.contains(typed);
        }
        std::lock_guard lock(state.llMutex);
        const auto typed = static_cast<xell_context_handle_t>(handle);
        return state.latency.contains(typed) || state.retiredLl.contains(typed);
    } catch (...) { return true; }
}

void ConfigureModel(uint32_t model, uint32_t hostModel) noexcept
{
    gConfiguredModel.store(model == 1 ? 1u : 0u, std::memory_order_release);
    gHostModel.store(hostModel <= 2 ? hostModel : 0u, std::memory_order_release);
}

// dev.43 motion fill: each skip or failure reason once per context (they repeat every frame).
void LogMotionFill(FgContext& context, const char* reason) noexcept
{
    try
    {
        if (!reason || !context.motionFillReasons.insert(reason).second || context.motionFillReasons.size() > 16) return;
        WriteLog("motion_fill_skipped", "\"generation\":"+std::to_string(context.generation)+",\"reason\":\""+reason+
            "\",\"disabled\":"+(context.motionFill && context.motionFill->Broken() ? "true" : "false"));
    }
    catch (...) {}
}

// Right before the game's Present (fgMutex held): the consumed motion tag's fill runs on our own list
// on the presentation queue, then our texture is tagged for that frame (UNTIL_NEXT_PRESENT, so XeFG
// needs no command list). The diagnostic capture records our texture as the frame's motion input.
void SubmitMotionFill(State& state, FgContext& context) noexcept
{
    try
    {
        uint32_t frame{};
        xefg_swapchain_d3d12_resource_data_t tag{};
        const char* reason{};
        const bool submitted = context.motionFill->Submit(frame, tag,
            [](ID3D12GraphicsCommandList* list, uint32_t id, const xefg_swapchain_d3d12_resource_data_t& data) {
                xess_capture::OnTagResource(list, id, data); }, reason);
        if (!submitted) { LogMotionFill(context, reason); return; }
        const auto result = state.api.xefgSwapChainD3D12TagFrameResource(context.handle, nullptr, frame, &tag);
        if (result != XEFG_SWAPCHAIN_RESULT_SUCCESS) LogMotionFill(context, "tag_rejected");
        if (!context.motionFillLogged || result != XEFG_SWAPCHAIN_RESULT_SUCCESS)
        {
            context.motionFillLogged = true;
            WriteLog("motion_fill_tag", "\"generation\":"+std::to_string(context.generation)+",\"frame\":"+std::to_string(frame)+
                ",\"width\":"+std::to_string(tag.resourceSize.x)+",\"height\":"+std::to_string(tag.resourceSize.y)+
                ",\"submitted\":"+std::to_string(context.motionFill->Submitted())+",\"result\":"+std::to_string(result));
        }
        else if (context.motionFill->Submitted()%3600 == 0)
            WriteLog("motion_fill_stats", "\"generation\":"+std::to_string(context.generation)+
                ",\"submitted\":"+std::to_string(context.motionFill->Submitted()));
    }
    catch (...) {}
}

// dev.47 HUD-less matcher: each skip or failure reason once per context.
void LogHudlessMatch(FgContext& context, const char* reason) noexcept
{
    try
    {
        if (!reason || !context.hudlessMatchReasons.insert(reason).second || context.hudlessMatchReasons.size() > 16) return;
        WriteLog("hudless_match_skipped", "\"generation\":"+std::to_string(context.generation)+",\"reason\":\""+reason+
            "\",\"disabled\":"+(context.hudlessMatch && context.hudlessMatch->Broken() ? "true" : "false"));
    }
    catch (...) {}
}

// Right before the game's Present (fgMutex held): the game's finished back buffer (HUD included, state
// PRESENT) and its consumed HUD-less input give this frame's curve; our list on the presentation queue
// writes the HUD-less input in the back buffer's encoding and our texture is tagged for that frame
// (UNTIL_NEXT_PRESENT). Our overlay draws after this, so the menu is UI to XeFG.
void SubmitHudless(State& state, FgContext& context) noexcept
{
    try
    {
        ComPtr<IDXGISwapChain3> chain;
        ComPtr<ID3D12Resource> backBuffer;
        if (context.chain && SUCCEEDED(context.chain.As(&chain)) && chain)
            chain->GetBuffer(chain->GetCurrentBackBufferIndex(), IID_PPV_ARGS(&backBuffer));
        uint32_t frame{};
        xefg_swapchain_d3d12_resource_data_t tag{};
        const char* reason{};
        const bool submitted = context.hudlessMatch->Submit(backBuffer.Get(), frame, tag,
            [](ID3D12GraphicsCommandList* list, uint32_t id, const xefg_swapchain_d3d12_resource_data_t& data) {
                xess_capture::OnTagResource(list, id, data); }, reason);
        if (!submitted) { LogHudlessMatch(context, reason); return; }
        const auto result = state.api.xefgSwapChainD3D12TagFrameResource(context.handle, nullptr, frame, &tag);
        if (result == XEFG_SWAPCHAIN_RESULT_SUCCESS) { context.hudlessMatchedKnown = true; context.hudlessMatchedFrame = frame; }
        else LogHudlessMatch(context, "tag_rejected");
        if (!context.hudlessMatchLogged || result != XEFG_SWAPCHAIN_RESULT_SUCCESS)
        {
            context.hudlessMatchLogged = true;
            WriteLog("hudless_match_tag", "\"generation\":"+std::to_string(context.generation)+",\"frame\":"+std::to_string(frame)+
                ",\"width\":"+std::to_string(tag.resourceSize.x)+",\"height\":"+std::to_string(tag.resourceSize.y)+
                ",\"format\":"+std::to_string(backBuffer ? int(backBuffer->GetDesc().Format) : 0)+
                ",\"submitted\":"+std::to_string(context.hudlessMatch->Submitted())+",\"result\":"+std::to_string(result));
        }
        else if (context.hudlessMatch->Submitted()%3600 == 0)
            WriteLog("hudless_match_stats", "\"generation\":"+std::to_string(context.generation)+
                ",\"submitted\":"+std::to_string(context.hudlessMatch->Submitted()));
    }
    catch (...) {}
}

void PreparePresent(void* swapchain, uint64_t generation) noexcept
{
    if (!swapchain || !generation) return;
    try
    {
        auto& state = Get();
        std::lock_guard lock(state.fgMutex);
        FgContext* context{};
        for (auto& [handle, candidate] : state.contexts)
            if (candidate->generation == generation && candidate->chain.Get() == swapchain)
            { context = candidate.get(); break; }
        if (!context || !context->initialized || context->closing || context->poisoned) return;
        if (context->motionFill && context->motionFill->Pending()) SubmitMotionFill(state, *context);
        if (context->hudlessMatch && context->hudlessMatch->Pending()) SubmitHudless(state, *context);
        // dev.45: UI composition from the frame's complete tags, once per frame.
        UpdateComposition(state, *context, context->tags.frame);
    }
    catch (...) { WriteLog("present_preparation_error", "\"reason\":\"present_preparation_failed\""); }
}

// dev.48 (the default since beta.1): the shader and model port on OptiScaler's provider image, then the
// frame-count unlock without the 2x-model site (XeFG remaps its 2x model to the ported model 16 for more than one
// generated frame). Before OptiScaler's first XeFG creation; kept for the process lifetime. Under fgMutex.
std::string PublishHostPort(State& state, HMODULE fg, HMODULE ll)
{
    if (!state.contexts.empty() || state.model || state.unlock) throw std::runtime_error("route_already_published");
    Configuration(state);
    auto shaders = std::make_unique<ShaderPort>(fg, Self(), state.root);
    for (const char* key : kReconstructionPrograms)
        if (!shaders->hasProgram(key)) throw std::runtime_error("reconstruction_program_pair_missing");
    auto model = std::make_unique<ModelPort>(fg, true, true);
    const auto names = AcquireUnlock(state, fg, ll, false);
    state.shaders = std::move(shaders);
    state.model = std::move(model);
    state.hostPorted = true;
    return names;
}

// dev.40: OptiScaler's own XeFG output (FGOutput xefg). Its contexts stay untracked: no port,
// capacity lease, pacing route, presentation change or UI attaches to them. On non-Intel GPUs
// the first one applies the frame-count unlock to the profiled pair (kept for the process
// lifetime); without it, or on any mismatch, XeFG runs stock (2x). dev.48/beta.1: unless the 2x model
// is selected, the ported MFG model is published first (on failure the 2x-model unlock applies as before).
void ApplyHostUnlock(State& state)
{
    std::call_once(state.hostUnlockOnce, [&] {
        try
        {
            const HMODULE fg = NativeFgModule(), ll = NativeLlModule();
            if (!GameCopyMatches(fg, ll)) throw std::runtime_error("host_pair_not_profiled");
            std::lock_guard lock(state.fgMutex);
            // modelchoice.1 (Dune Awakening): the game's own XeFG context came first and the tracked route
            // published the port on this image. Port and unlock are process-wide on the image, so OptiScaler's
            // contexts already run on them: adopt both for the process lifetime. Rejecting them left the
            // host "unlocked":false, and the tracked context's destruction then restored the model under it.
            if (state.model && state.unlock && state.unlockFg == fg && !state.unlock->Keeps2xModel())
            {
                state.hostUnlocked = true;
                state.hostPorted = true;
                WriteLog("host_unlock", "\"shared\":\"tracked_route\",\"generated\":"+std::to_string(kGeneratedCapacity)+
                    ",\"model\":\"ported_mfg_model\""+UnlockXellField(ll)+ModelChoiceFields(TrackedModelChoice()));
                return;
            }
            if (HostPortRequested())
            {
                try
                {
                    const auto names = PublishHostPort(state, fg, ll);
                    state.hostUnlocked = true;
                    WriteLog("host_unlock", "\"applied\":["+names+"],\"generated\":"+std::to_string(kGeneratedCapacity)+
                        ",\"model\":\"ported_mfg_model\""+UnlockXellField(ll)+ModelChoiceFields(HostModelChoice()));
                    return;
                }
                catch (const std::exception& error)
                {
                    WriteLog("host_model_port_rejected", "\"reason\":\""+escape(error.what())+"\",\"fallback\":\"xefg_2x_model\"");
                }
            }
            const auto names = AcquireUnlock(state, fg, ll, true);
            state.hostUnlocked = true;
            WriteLog("host_unlock", "\"applied\":["+names+"],\"generated\":"+std::to_string(kGeneratedCapacity)+
                ",\"model\":\"xefg_2x_model\""+UnlockXellField(ll)+ModelChoiceFields(HostModelChoice()));
        }
        catch (const std::exception& error)
        {
            WriteLog("host_unlock_rejected", "\"reason\":\""+escape(error.what())+"\"");
        }
    });
}

xefg_swapchain_result_t HostCreateFg(State& state, ID3D12Device* device, xefg_swapchain_handle_t* output)
{
    const bool intel = adapter_support::VendorOf(device) == adapter_support::kIntelVendor;
    if (!intel) ApplyHostUnlock(state);
    const auto result = original.xefgSwapChainD3D12CreateContext(device, output);
    std::lock_guard lock(state.fgMutex);
    if (result == XEFG_SWAPCHAIN_RESULT_SUCCESS && *output && !state.contexts.contains(*output))
    {
        state.hostFg[*output] = {device, ++state.generation, false};
        state.retiredFg.erase(*output);
    }
    WriteLog("host_context_create", "\"result\":"+std::to_string(result)+",\"handle\":"+
        std::to_string(uintptr_t(*output))+",\"unlocked\":"+(state.hostUnlocked ? "true" : "false")+
        ",\"intel\":"+(intel ? "true" : "false"));
    // modelchoice.1: the status shows the model the host unlock applied (xessModelActive).
    Publish(state);
    return result;
}

// dev.41: OptiScaler's own XeFG paces only the first generated output of a batch; at 3x-6x
// the others went out back to back (PresentMon, 10 Oct). After its initialization succeeds,
// a host context gets the plain sdk_pacing owner: extra outputs go through XeFG's own pacing
// wait and each deadline follows the batch interval (NativeDeadline).
// RTXMFG_XESS_OPTISCALER_PACING=0 opts out.
// dev.48: with the capture enabled the owner is also observed, so the final-present capture sees
// OptiScaler's outputs (its input tags go straight to the provider and are not recorded).
bool HostPacingEnabled() noexcept
{
    static const bool enabled = [] {
        char value[8]{};
        const DWORD length = GetEnvironmentVariableA("RTXMFG_XESS_OPTISCALER_PACING", value, DWORD(sizeof(value)));
        return !(length == 1 && value[0] == '0');
    }();
    return enabled;
}

// scenechange.1: OptiScaler.ini [XeFG] XeFGSceneChangeThreshold = 0..1 sets XeFG's scene-change sensitivity on
// OptiScaler's XeFG (xefgSwapChainSetSceneChangeThreshold; XeFG's default is 0.7, lower = fewer interpolations
// dropped as scene cuts on fast camera turns). Absent or auto leaves XeFG's own value; OptiScaler never sets it.
void ApplyHostSceneChangeThreshold(xefg_swapchain_handle_t handle)
{
    float threshold{};
    std::wstring setting;
    const bool configured = optiscaler_host::ReadXefgSceneChangeThreshold(threshold, setting);
    if (!configured)
    {
        if (!setting.empty())
            WriteLog("host_scene_change_threshold", "\"handle\":"+std::to_string(uintptr_t(handle))+
                ",\"applied\":false,\"reason\":\"invalid_setting\",\"setting\":\""+escape(utf8(setting))+"\"");
        return;
    }
    const auto result = original.xefgSwapChainSetSceneChangeThreshold
        ? original.xefgSwapChainSetSceneChangeThreshold(handle, threshold) : XEFG_SWAPCHAIN_RESULT_ERROR_UNKNOWN;
    char value[16]{};
    sprintf_s(value, "%.3f", threshold);
    WriteLog("host_scene_change_threshold", "\"handle\":"+std::to_string(uintptr_t(handle))+",\"threshold\":"+value+
        ",\"applied\":"+(result == XEFG_SWAPCHAIN_RESULT_SUCCESS ? "true" : "false")+",\"result\":"+std::to_string(result));
}

template<class Initialize>
xefg_swapchain_result_t HostInitFg(State& state, xefg_swapchain_handle_t handle, Initialize initialize)
{
    const auto result = initialize();
    if (result != XEFG_SWAPCHAIN_RESULT_SUCCESS) return result;
    {
        std::lock_guard lock(state.fgMutex);
        if (!state.hostFg.contains(handle)) return result;
    }
    ApplyHostSceneChangeThreshold(handle);
    std::lock_guard lock(state.fgMutex);
    const auto it = state.hostFg.find(handle);
    if (it == state.hostFg.end()) return result;
    auto& host = it->second;
    if (host.paced) { sdk_pacing::Unregister(handle, host.generation); host.paced = false; } // initialized again
    std::string failure;
    const bool intel = adapter_support::VendorOf(host.device.Get()) == adapter_support::kIntelVendor;
    const HMODULE fg = NativeFgModule();
    if (!HostPacingEnabled()) failure = "disabled";
    else if (intel) failure = "intel_adapter";
    else if (!fg) failure = "no_provider_module";
    else if (sdk_pacing::Install(fg, failure))
        host.paced = sdk_pacing::Register(handle, host.generation, host.device.Get(), failure, xess_capture::Enabled());
    WriteLog("host_pacing_owner", "\"generation\":"+std::to_string(host.generation)+",\"handle\":"+
        std::to_string(uintptr_t(handle))+",\"registered\":"+(host.paced ? "true" : "false")+
        ",\"observed\":"+(host.paced && xess_capture::Enabled() ? "true" : "false")+
        (failure.empty() ? "" : ",\"reason\":\""+escape(failure)+"\""));
    return result;
}

xefg_swapchain_result_t HostDestroyFg(State& state, xefg_swapchain_handle_t handle)
{
    uint64_t generation{};
    {
        std::lock_guard lock(state.fgMutex);
        const auto it = state.hostFg.find(handle);
        if (it != state.hostFg.end())
        {
            generation = it->second.generation;
            if (it->second.paced) { sdk_pacing::Unregister(handle, generation); it->second.paced = false; }
        }
    }
    const auto result = original.xefgSwapChainDestroy(handle);
    std::lock_guard lock(state.fgMutex);
    if (result == XEFG_SWAPCHAIN_RESULT_SUCCESS) state.hostFg.erase(handle);
    WriteLog("host_context_destroy", "\"generation\":"+std::to_string(generation)+",\"handle\":"+
        std::to_string(uintptr_t(handle))+",\"result\":"+std::to_string(result));
    return result;
}

xefg_swapchain_result_t Hook_xefgSwapChainD3D12CreateContext(ID3D12Device* device, xefg_swapchain_handle_t* output)
{
    if (!device || !output) return kArgument;
    *output = nullptr;
    auto& state = Get();
    if (tHostCall) return HostCreateFg(state, device, output);
    xefg_swapchain_handle_t created{};
    try
    {
        const UINT vendor = adapter_support::VendorOf(device);
        if (vendor)
        {
            std::lock_guard lock(state.fgMutex);
            state.display.adapterVendor = vendor;
            Publish(state);
        }
        // Intel's provider keeps its own generation (beta.8: no native control).
        if (vendor == adapter_support::kIntelVendor) return original.xefgSwapChainD3D12CreateContext(device, output);
        try { VerifyAdapter(device); }
        catch (const std::exception& error)
        {
            WriteLog("native_adapter_fallback", "\"reason\":\""+escape(error.what())+"\"");
            return original.xefgSwapChainD3D12CreateContext(device, output);
        }
        if (!EnsureRuntime(state))
        {
            std::lock_guard lock(state.fgMutex);
            Failure(state, state.loadFailure.c_str());
            return original.xefgSwapChainD3D12CreateContext(device, output);
        }
        std::lock_guard lock(state.fgMutex);
        if (state.contexts.size() >= 8 || state.uncertainFgLifetime) return XEFG_SWAPCHAIN_RESULT_ERROR_UNSUPPORTED;
        state.display.adapter = VerifyAdapter(device);
        state.display.provider = utf8(ModulePath(state.fgModule).wstring());
        PreparePort(state);
        auto context = std::make_unique<FgContext>();
        context->device = device;
        context->generation = ++state.generation;
        // The unlock holds the provider override at the full portable capacity (6x) for the process lifetime.
        state.uncertainFgLifetime = true;
        const auto result = state.api.xefgSwapChainD3D12CreateContext(device, &created);
        WriteLog("context_create", "\"result\":"+std::to_string(result)+",\"handle\":"+std::to_string(uintptr_t(created))+
            ",\"generation\":"+std::to_string(context->generation));
        if (result != XEFG_SWAPCHAIN_RESULT_SUCCESS || !created)
        {
            FinishFailedCreate(state, created, result);
            Failure(state, state.uncertainFgLifetime ? "XeSS creation cleanup was not confirmed; restart the game."
                : "XeSS context creation failed.", result);
            return result == XEFG_SWAPCHAIN_RESULT_SUCCESS ? kInvalid : result;
        }
        context->handle = created;
        if (state.contexts.contains(created)) throw std::runtime_error("provider_reused_live_context");
        state.hostFg.erase(created); // a reused value of a destroyed host context
        auto* record = context.get();
        state.contexts.emplace(created, std::move(context));
        state.retiredFg.erase(created);
        state.api.xefgSwapChainSetLoggingCallback(created, XEFG_SWAPCHAIN_LOGGING_LEVEL_WARNING, FgLog, record);
        state.display.status = "XeSS context created; waiting for swap-chain initialization.";
        Publish(state, record);
        state.uncertainFgLifetime = false;
        *output = created;
        return result;
    }
    catch (const std::exception& error)
    {
        std::lock_guard lock(state.fgMutex);
        if (state.contexts.empty()) RestoreIdlePort(state);
        Failure(state, error.what());
        return XEFG_SWAPCHAIN_RESULT_ERROR_UNSUPPORTED;
    }
}

static bool HeapFits(FgContext& context, ID3D12Heap* heap, uint64_t offset, uint64_t bytes)
{
    if (!heap) return offset == 0;
    ComPtr<ID3D12Device> owner;
    if (heap->GetDevice(IID_PPV_ARGS(&owner)) != S_OK || !SameDevice(context.device.Get(), owner.Get())) return false;
    const auto desc = heap->GetDesc();
    return desc.Properties.Type == D3D12_HEAP_TYPE_DEFAULT && offset % D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT == 0
        && offset <= desc.SizeInBytes && bytes <= desc.SizeInBytes-offset;
}
static bool HeapPair(FgContext& context, ID3D12Heap* buffer, uint64_t bo, ID3D12Heap* texture, uint64_t to,
    const xefg_swapchain_properties_t& properties)
{
    if (!HeapFits(context, buffer, bo, properties.tempBufferHeapSize)
        || !HeapFits(context, texture, to, properties.tempTextureHeapSize)) return false;
    if (buffer && texture) {
        ComPtr<IUnknown> a,b;
        if (buffer->QueryInterface(IID_PPV_ARGS(&a)) != S_OK || texture->QueryInterface(IID_PPV_ARGS(&b)) != S_OK) return false;
        if (a.Get() == b.Get() && !(bo+properties.tempBufferHeapSize <= to || to+properties.tempTextureHeapSize <= bo)) return false;
    }
    return true;
}

static xefg_swapchain_result_t InitializeFg(xefg_swapchain_handle_t handle,
    ID3D12CommandQueue* queue, const xefg_swapchain_d3d12_init_params_t* input,
    HWND window, const DXGI_SWAP_CHAIN_DESC1* desc, const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* full, IDXGIFactory2* factory)
{
    auto& state = Get();
    std::lock_guard lock(state.fgMutex);
    auto* context = FindFg(state, handle);
    WriteLog("context_init_request", std::string("\"tracked\":")+(context ? "true" : "false")+
        ",\"from_desc\":"+(desc ? "true" : "false")+",\"queue\":"+(queue ? "true" : "false")+
        ",\"window\":"+(window ? "true" : "false")+",\"factory\":"+(factory ? "true" : "false")+
        ",\"app_swapchain\":"+(input && input->pApplicationSwapChain ? "true" : "false")+
        ",\"max_interpolated\":"+(input ? std::to_string(input->maxInterpolatedFrames) : "null")+
        ",\"flags\":"+(input ? std::to_string(input->initFlags) : "null")+
        ",\"ui_mode\":"+(input ? std::to_string(input->uiMode) : "null")+
        (desc ? ",\"width\":"+std::to_string(desc->Width)+",\"height\":"+std::to_string(desc->Height)+
            ",\"format\":"+std::to_string(desc->Format) : std::string()));
    if (!context) {
        if (state.retiredFg.contains(handle)) return kInvalid;
        const auto result = desc ? original.xefgSwapChainD3D12InitFromSwapChainDesc(handle, window, desc, full, queue, factory, input)
            : original.xefgSwapChainD3D12InitFromSwapChain(handle, queue, input);
        WriteLog("native_context_init", "\"result\":"+std::to_string(result));
        return result;
    }
    const char* rejected = !input ? "missing_parameters" : !queue ? "missing_queue"
        : !desc && !input->pApplicationSwapChain ? "missing_application_swapchain"
        : desc && !factory ? "missing_factory" : desc && !window ? "missing_window"
        // Intel: pApplicationSwapChain "will not be used" for the descriptor
        // route; a stale value there is the caller's, not a reason to refuse.
        : context->initialized ? "already_initialized" : context->closing ? "closing"
        : context->poisoned ? "poisoned" : nullptr;
    if (rejected)
    {
        WriteLog("context_init_rejected", std::string("\"reason\":\"")+rejected+"\",\"generation\":"+
            std::to_string(context->generation));
        return kArgument;
    }
    try
    {
        ComPtr<ID3D12Device> queueDevice, chainDevice;
        checked(queue->GetDevice(IID_PPV_ARGS(&queueDevice)), "init_queue_device");
        if (!desc) checked(input->pApplicationSwapChain->GetDevice(IID_PPV_ARGS(&chainDevice)), "init_chain_device");
        if (queue->GetDesc().Type != D3D12_COMMAND_LIST_TYPE_DIRECT
            || !SameDevice(context->device.Get(), queueDevice.Get()) || (!desc && !SameDevice(context->device.Get(), chainDevice.Get())))
            throw std::runtime_error("swapchain_queue_device_identity_rejected");
        if ((input->initFlags & ~uint32_t(0x3f)) || input->uiMode < 0 || input->uiMode > 5
            || input->creationNodeMask > 1 || input->visibleNodeMask > 1
            || !input->maxInterpolatedFrames || (input->maxInterpolatedFrames > kGeneratedCapacity
                && input->maxInterpolatedFrames != XEFG_SWAPCHAIN_USE_MAX_SUPPORTED_INTERPOLATED_FRAMES))
            throw std::runtime_error("unsupported_initialization_parameters");
        DXGI_SWAP_CHAIN_DESC description{};
        if (desc) {
            description.BufferDesc.Width=desc->Width; description.BufferDesc.Height=desc->Height;
            description.BufferDesc.Format=desc->Format;
        } else checked(input->pApplicationSwapChain->GetDesc(&description), "init_swapchain_description");
        // dev.46: the model (state.model) is chosen at creation; pacing is the plain owner.
        auto params = *input;
        params.maxInterpolatedFrames = kGeneratedCapacity;
        if (desc) params.pApplicationSwapChain = nullptr; // Intel: must be NULL on the descriptor route.
        // An application cache may contain pipelines compiled before our model
        // publication. The private context builds and owns its own pipelines
        // (both routes, as in dev.41).
        params.pPipelineLibrary = nullptr;
        xefg_swapchain_properties_t properties{};
        auto result = state.api.xefgSwapChainD3D12GetProperties(handle, &params, description.BufferDesc.Width,
            description.BufferDesc.Height, description.BufferDesc.Format, &properties);
        if (result != XEFG_SWAPCHAIN_RESULT_SUCCESS || properties.maxSupportedInterpolations < kGeneratedCapacity)
        {
            Report(state, *context, result, "capacity query");
            return result == XEFG_SWAPCHAIN_RESULT_SUCCESS ? XEFG_SWAPCHAIN_RESULT_ERROR_UNSUPPORTED : result;
        }
        if (!HeapPair(*context, params.pTempBufferHeap, params.bufferHeapOffset,
            params.pTempTextureHeap, params.textureHeapOffset, properties))
            throw std::runtime_error("external_heap_capacity_or_owner_rejected");
        single_overlay::InstallKnownModules();
        context->presentationQueue = queue;
        context->initializationAttempted = true;
        result = desc ? state.api.xefgSwapChainD3D12InitFromSwapChainDesc(handle, window, desc, full, queue, factory, &params)
            : state.api.xefgSwapChainD3D12InitFromSwapChain(handle, queue, &params);
        WriteLog("context_init", "\"result\":"+std::to_string(result)+",\"generation\":"+std::to_string(context->generation)+
            ",\"requested_capacity\":"+std::to_string(kGeneratedCapacity)+",\"width\":"+std::to_string(description.BufferDesc.Width)+
            ",\"height\":"+std::to_string(description.BufferDesc.Height)+",\"format\":"+
            std::to_string(description.BufferDesc.Format)+",\"flags\":"+std::to_string(params.initFlags)+
            ",\"ui_mode\":"+std::to_string(params.uiMode)+
            ",\"route\":\""+(state.model ? "ported_mfg_model" : "xefg_2x_model")+"\",\"pacing\":\"plain\"");
        if (result != XEFG_SWAPCHAIN_RESULT_SUCCESS)
        {
            context->poisoned = true;
            context->initializationFailure = result;
            state.api.xefgSwapChainSetEnabled(handle, 0);
            Report(state, *context, result, "initialization");
            return result;
        }
        xefg_swapchain_d3d12_init_params_t applied{};
        result = state.api.xefgSwapChainD3D12GetInitializationParameters(handle, &applied);
        if (result != XEFG_SWAPCHAIN_RESULT_SUCCESS || applied.maxInterpolatedFrames != kGeneratedCapacity)
        {
            context->poisoned = true;
            state.api.xefgSwapChainSetEnabled(handle, 0);
            Failure(state, "XeSS allocation capacity was not confirmed.");
            return XEFG_SWAPCHAIN_RESULT_ERROR_UNSUPPORTED;
        }
        context->capacity = applied.maxInterpolatedFrames;
        context->initFlags = applied.initFlags;
        std::string pacingFailure;
        // dev.42: the plain owner dev.41 gave OptiScaler's own XeFG: each extra output goes through
        // XeFG's own pacing wait and each deadline follows the batch interval (DLSS-G's CPU pacer
        // rules, NativeDeadline). With the capture enabled it is also observed (sdk_pacing::Register).
        if (!sdk_pacing::Register(handle, context->generation, context->device.Get(), pacingFailure,
            xess_capture::Enabled()))
        {
            context->poisoned = true;
            context->initializationFailure = XEFG_SWAPCHAIN_RESULT_ERROR_UNSUPPORTED;
            state.api.xefgSwapChainSetEnabled(handle, 0);
            Failure(state, pacingFailure.empty() ? "pacing_owner_not_confirmed" : pacingFailure.c_str());
            return XEFG_SWAPCHAIN_RESULT_ERROR_UNSUPPORTED;
        }
        context->width = description.BufferDesc.Width;
        context->height = description.BufferDesc.Height;
        context->uiMode = params.uiMode;
        context->gameGenerated = input->maxInterpolatedFrames == XEFG_SWAPCHAIN_USE_MAX_SUPPORTED_INTERPOLATED_FRAMES
            ? kGeneratedCapacity : input->maxInterpolatedFrames;
        // dev.43: GPU resources and the shader come with the first motion tag (its shape).
        context->motionFill = std::make_unique<xess_motion_fill::Filler>(context->device.Get(), queue);
        // dev.47: its shaders come with the first HUD-less tag, its textures with the first back buffer.
        context->hudlessMatch = std::make_unique<xess_hudless_match::Matcher>(context->device.Get(), queue);
        context->initialized = true;
        ApplyPending(state, *context);
        Publish(state, context);
        return context->poisoned ? XEFG_SWAPCHAIN_RESULT_ERROR_UNSUPPORTED : XEFG_SWAPCHAIN_RESULT_SUCCESS;
    }
    catch (const std::exception& error)
    {
        if (context->initializationAttempted)
        {
            context->poisoned = true;
            context->initializationFailure = XEFG_SWAPCHAIN_RESULT_ERROR_UNSUPPORTED;
        }
        Failure(state, error.what());
        Publish(state, context);
        return XEFG_SWAPCHAIN_RESULT_ERROR_UNSUPPORTED;
    }
}

xefg_swapchain_result_t Hook_xefgSwapChainD3D12InitFromSwapChain(xefg_swapchain_handle_t handle,
    ID3D12CommandQueue* queue, const xefg_swapchain_d3d12_init_params_t* input)
{
    if (tHostCall && HostOwnedHandle(handle, true))
        return HostInitFg(Get(), handle, [&] { return original.xefgSwapChainD3D12InitFromSwapChain(handle, queue, input); });
    return InitializeFg(handle, queue, input, nullptr, nullptr, nullptr, nullptr);
}
xefg_swapchain_result_t Hook_xefgSwapChainD3D12InitFromSwapChainDesc(xefg_swapchain_handle_t handle,
    HWND window, const DXGI_SWAP_CHAIN_DESC1* desc, const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* full,
    ID3D12CommandQueue* queue, IDXGIFactory2* factory, const xefg_swapchain_d3d12_init_params_t* input)
{
    if (tHostCall && HostOwnedHandle(handle, true))
        return HostInitFg(Get(), handle, [&] {
            return original.xefgSwapChainD3D12InitFromSwapChainDesc(handle, window, desc, full, queue, factory, input); });
    if (!desc) return kArgument;
    return InitializeFg(handle, queue, input, window, desc, full, factory);
}

xefg_swapchain_result_t Hook_xefgSwapChainSetEnabled(xefg_swapchain_handle_t handle, uint32_t enabled)
{
    auto& state = Get(); std::lock_guard lock(state.fgMutex);
    auto* context = FindFg(state, handle);
    if (!context)
    {
        // Native contexts still tell the menu the game uses XeSS-FG.
        if (state.retiredFg.contains(handle)) return kInvalid;
        const auto result = original.xefgSwapChainSetEnabled(handle, enabled);
        if (result == XEFG_SWAPCHAIN_RESULT_SUCCESS)
        {
            state.nativeEnabled.store(enabled != 0, std::memory_order_release);
            state.nativeActivityTick.store(GetTickCount64(), std::memory_order_release);
        }
        return result;
    }
    if (context->closing || (enabled && (!context->initialized || context->poisoned)))
        return XEFG_SWAPCHAIN_RESULT_ERROR_WRONG_CALL_ORDER;
    if (enabled)
    {
        ApplyPending(state, *context);
        if (context->poisoned) return XEFG_SWAPCHAIN_RESULT_ERROR_WRONG_CALL_ORDER;
    }
    const auto result = state.api.xefgSwapChainSetEnabled(handle, enabled);
    if (result == XEFG_SWAPCHAIN_RESULT_SUCCESS)
    {
        if (context->enabled != (enabled != 0)) ResetPacing(*context);
        context->enabled = enabled != 0;
        context->gameEnabled = enabled != 0;
    }
    WriteLog("set_enabled", "\"enabled\":"+std::to_string(enabled)+",\"result\":"+std::to_string(result));
    Report(state, *context, result, "enable");
    return result;
}

xefg_swapchain_result_t Hook_xefgSwapChainSetPresentId(xefg_swapchain_handle_t handle, uint32_t id)
{
    auto& state = Get(); std::lock_guard lock(state.fgMutex);
    auto* context = FindFg(state, handle);
    if (!context)
    {
        if (state.retiredFg.contains(handle)) return kInvalid;
        state.nativeActivityTick.store(GetTickCount64(), std::memory_order_release);
        return original.xefgSwapChainSetPresentId(handle, id);
    }
    if (context->closing) return kInvalid;
    if (!context->initialized || context->poisoned) return XEFG_SWAPCHAIN_RESULT_ERROR_WRONG_CALL_ORDER;
    ApplyPending(state, *context);
    if (context->poisoned) return XEFG_SWAPCHAIN_RESULT_ERROR_WRONG_CALL_ORDER;
    // dev.45: UI composition is decided once per frame at the game's Present (PreparePresent); here
    // the new frame's tags were not in yet and composition went off and on twice every frame.
    const auto result = state.api.xefgSwapChainSetPresentId(handle, id);
    if (result == XEFG_SWAPCHAIN_RESULT_SUCCESS)
    {
        context->presentId = id; context->presentIdKnown = true;
        state.display.lastPresentId = id; ++state.display.appFrames;
    }
    Report(state, *context, result, "frame boundary");
    return result;
}

// dev.42: the matrix slot fix is on unless RTXMFG_XESS_MATRIX_FIX=0 (read once). It only acts
// when the slots classify as swapped (projection in the view slot, view in the projection slot).
bool MatrixFixDefault() noexcept
{
    static const bool enabled = [] {
        char value[8]{};
        const DWORD length = GetEnvironmentVariableA("RTXMFG_XESS_MATRIX_FIX", value, DWORD(sizeof(value)));
        return !(length == 1 && value[0] == '0');
    }();
    return enabled;
}

// dev.43: motion vectors for the game's own XeFG (xess_mod.h InputDiagnostics::motionVectors).
// RTXMFG_XESS_MV_FILL: 0 = the game's own, 2 = all zero (diagnostic), otherwise the filled marker.
uint32_t MotionFillDefault() noexcept
{
    static const uint32_t mode = [] {
        char value[8]{};
        const DWORD length = GetEnvironmentVariableA("RTXMFG_XESS_MV_FILL", value, DWORD(sizeof(value)));
        if (length == 1 && value[0] == '0') return uint32_t(xess_motion_fill::Mode::eGame);
        if (length == 1 && value[0] == '2') return uint32_t(xess_motion_fill::Mode::eZero);
        return uint32_t(xess_motion_fill::Mode::eFill);
    }();
    return mode;
}

// dev.44 UI composition override; dev.45: on by default (composition off, XeFG 3.0.2's own default).
// With Witcher's composition (ui_mode 4, 4K HUDless) every generated output was a copy of the
// nearest real frame (dev.38-44 captures); off, they interpolate (dev.44 capture B/C).
// dev.47: the copies came from the HUD-less input's encoding (linear pre-tonemap vs the HDR10 back
// buffer); with the matcher composition is the default again. RTXMFG_XESS_UI_COMPOSITION=off|0 keeps
// it off. Read once.
bool UiCompositionOffDefault() noexcept
{
    static const bool off = [] {
        char value[8]{};
        const DWORD length = GetEnvironmentVariableA("RTXMFG_XESS_UI_COMPOSITION", value, DWORD(sizeof(value)));
        return (length == 1 && value[0] == '0') || (length == 3 && !_stricmp(value, "off"));
    }();
    return off;
}

// dev.47: the HUD-less input converted to the back buffer's encoding (xess_mod.h InputDiagnostics::hudless).
// RTXMFG_XESS_HUDLESS=game|0 forwards the game's own HUD-less input. Read once.
uint32_t HudlessMatchDefault() noexcept
{
    static const uint32_t mode = [] {
        char value[8]{};
        const DWORD length = GetEnvironmentVariableA("RTXMFG_XESS_HUDLESS", value, DWORD(sizeof(value)));
        const bool game = (length == 1 && value[0] == '0') || (length == 4 && !_stricmp(value, "game"));
        return uint32_t(game ? xess_hudless_match::Mode::eGame : xess_hudless_match::Mode::eMatch);
    }();
    return mode;
}

// The matrix slot fix (dev.42) and the motion-vector source (dev.43), read once.
static const bool matrixFixEnabled = MatrixFixDefault();
static const uint32_t motionMode = MotionFillDefault();
static std::atomic<uint64_t> matrixSwaps{};

// dev.37: what the game passes as XeFG frame constants. The OptiScaler XeFG input found
// Witcher passing a view matrix plus a combined view-projection matrix where XeFG
// expects view + projection (row-major). Logged for the first 3 frames, then every 600.
const char* MatrixKind(const float (&m)[16]) noexcept
{
    const auto zero = [](float value) { return std::abs(value) < 1e-4f; };
    if (zero(m[15]) && (std::abs(std::abs(m[11])-1.0f) < 1e-3f || std::abs(std::abs(m[14])-1.0f) < 1e-3f)) return "projection";
    const bool rigid = std::abs(m[15]-1.0f) < 1e-4f
        && ((zero(m[3]) && zero(m[7]) && zero(m[11])) || (zero(m[12]) && zero(m[13]) && zero(m[14])));
    return rigid ? "view" : "other";
}
void LogFrameConstants(const FgContext& context, uint32_t id, const xefg_swapchain_frame_constant_data_t& c) noexcept
{
    static std::atomic<uint64_t> calls{};
    const auto count = calls.fetch_add(1, std::memory_order_relaxed)+1;
    if (count > 3 && count%600) return;
    try
    {
        const auto list = [](const float* values, size_t n) {
            std::string text = "[";
            for (size_t i = 0; i < n; ++i)
            {
                char number[24]{};
                if (std::isfinite(values[i])) sprintf_s(number, "%.6g", values[i]); else strcpy_s(number, "null");
                text += (i ? "," : "")+std::string(number);
            }
            return text+"]";
        };
        const float jitter[2]{c.jitterOffsetX, c.jitterOffsetY}, scale[2]{c.motionVectorScaleX, c.motionVectorScaleY};
        WriteLog("frame_constants", "\"generation\":"+std::to_string(context.generation)+",\"frame\":"+std::to_string(id)+
            ",\"call\":"+std::to_string(count)+",\"init_flags\":"+std::to_string(context.initFlags)+
            ",\"view_kind\":\""+MatrixKind(c.viewMatrix)+"\",\"projection_kind\":\""+MatrixKind(c.projectionMatrix)+
            "\",\"view\":"+list(c.viewMatrix, 16)+",\"projection\":"+list(c.projectionMatrix, 16)+
            ",\"jitter\":"+list(jitter, 2)+",\"mv_scale\":"+list(scale, 2)+",\"reset\":"+std::to_string(c.resetHistory)+
            ",\"frame_time\":"+list(&c.frameRenderTime, 1));
    }
    catch (...) {}
}

xefg_swapchain_result_t Hook_xefgSwapChainTagFrameConstants(xefg_swapchain_handle_t handle, uint32_t id,
    const xefg_swapchain_frame_constant_data_t* constants)
{
    auto& state = Get(); std::lock_guard lock(state.fgMutex);
    auto* context = FindFg(state, handle);
    if (!context) return state.retiredFg.contains(handle) ? kInvalid : original.xefgSwapChainTagFrameConstants(handle, id, constants);
    if (context->closing || !constants) return kArgument;
    if (!context->initialized || context->poisoned) return XEFG_SWAPCHAIN_RESULT_ERROR_WRONG_CALL_ORDER;
    // Both SDK headers require milliseconds. The verified Cyberpunk and Witcher
    // builds supply seconds; since dev.33 every other game's units are detected per
    // context (frame_time_profile::UnitDetector; RTXMFG_XESS_FRAME_TIME_UNITS forces
    // them). Only our tracked context receives a copy; caller memory stays untouched.
    LARGE_INTEGER tagQpc{}, tagFrequency{};
    QueryPerformanceCounter(&tagQpc); QueryPerformanceFrequency(&tagFrequency);
    const double tagGapMs = context->lastConstantsQpc && tagQpc.QuadPart > context->lastConstantsQpc && tagFrequency.QuadPart > 0
        ? double(tagQpc.QuadPart-context->lastConstantsQpc)*1000.0/double(tagFrequency.QuadPart) : 0.0;
    const auto profile = WitcherProfile() ? frame_time_profile::Profile::eWitcher3Seconds
        : (CyberpunkProfile() ? frame_time_profile::Profile::eCyberpunkSeconds
            : frame_time_profile::Profile::eUnknown);
    const auto forced = ForcedFrameTimeUnits();
    frame_time_profile::Units units{};
    if (forced != frame_time_profile::Units::eUndecided) units = forced;
    else if (frame_time_profile::IsSecondsProfile(profile)) units = frame_time_profile::Units::eSeconds;
    else
    {
        units = context->units.Observe(constants->frameRenderTime, tagGapMs);
        const auto decided = context->units.Decided();
        if (decided != frame_time_profile::Units::eUndecided && !context->unitsLogged)
        {
            context->unitsLogged = true;
            WriteLog("frame_time_units_detected", "\"generation\":"+std::to_string(context->generation)+
                ",\"units\":\""+frame_time_profile::Name(decided)+"\",\"frame\":"+std::to_string(id)+
                ",\"source\":"+(std::isfinite(constants->frameRenderTime) ? std::to_string(constants->frameRenderTime) : "null")+
                ",\"tag_gap_ms\":"+std::to_string(tagGapMs)+",\"votes\":"+std::to_string(context->units.Votes()));
        }
    }
    const auto correction = frame_time_profile::NormalizeUnits(constants->frameRenderTime, units);
    auto adjusted = *constants;
    adjusted.frameRenderTime = correction.milliseconds;
    // dev.38: put the view and projection back in their slots when the game passes them
    // swapped (Witcher, dev.37 frame_constants: view slot = reversed-Z projection, projection
    // slot = row-major view). Only XeFG's copy changes.
    if (matrixFixEnabled && !strcmp(MatrixKind(adjusted.viewMatrix), "projection")
        && !strcmp(MatrixKind(adjusted.projectionMatrix), "view"))
    {
        float swap[16];
        memcpy(swap, adjusted.viewMatrix, sizeof(swap));
        memcpy(adjusted.viewMatrix, adjusted.projectionMatrix, sizeof(swap));
        memcpy(adjusted.projectionMatrix, swap, sizeof(swap));
        if (matrixSwaps.fetch_add(1, std::memory_order_acq_rel) == 0)
            WriteLog("frame_constants_slot_fix", "\"generation\":"+std::to_string(context->generation)+",\"frame\":"+std::to_string(id));
    }
    const float seconds = constants->frameRenderTime;
    const auto result = state.api.xefgSwapChainTagFrameConstants(handle, id, &adjusted);
    LogFrameConstants(*context, id, adjusted);
    if (result == XEFG_SWAPCHAIN_RESULT_SUCCESS)
    {
        xess_capture::OnFrameConstants(id, adjusted);
        if (context->motionFill) context->motionFill->OnConstants(id, adjusted.viewMatrix, adjusted.projectionMatrix);
    }
    if (++context->constantsAttempts <= 3 && correction.converted)
        WriteLog("frame_time_units", "\"generation\":"+std::to_string(context->generation)+
            ",\"profile\":\""+(frame_time_profile::IsSecondsProfile(profile) ? frame_time_profile::Name(profile)
                : forced != frame_time_profile::Units::eUndecided ? "forced" : "detected")+
            "\",\"frame\":"+std::to_string(id)+",\"source_seconds\":"+
            (correction.validInput ? std::to_string(seconds) : "null")+",\"provider_milliseconds\":"+
            std::to_string(adjusted.frameRenderTime)+",\"result\":"+std::to_string(result));
    RecordGameCadence(*context, correction.validInput ? double(adjusted.frameRenderTime) : NAN, id);
    context->constantsAvailable = result == XEFG_SWAPCHAIN_RESULT_SUCCESS;
    if (context->constantsAvailable)
    {
        context->constantsFrame = id;
        context->frameConstants = adjusted;
        ++state.display.constantsCalls;
    }
    if (result < 0) Report(state, *context, result, "frame constants");
    return result;
}

xefg_swapchain_result_t Hook_xefgSwapChainD3D12TagFrameResource(xefg_swapchain_handle_t handle,
    ID3D12CommandList* commandList, uint32_t id, const xefg_swapchain_d3d12_resource_data_t* resource)
{
    auto& state = Get(); std::lock_guard lock(state.fgMutex);
    auto* context = FindFg(state, handle);
    if (!context)
    {
        if (++state.foreignResourceCalls <= 8)
            WriteLog("untracked_resource_tag", "\"handle\":"+std::to_string(uintptr_t(handle))+
                ",\"frame\":"+std::to_string(id)+",\"type\":"+std::to_string(resource ? int(resource->type) : -1)+
                ",\"retired\":"+std::string(state.retiredFg.contains(handle) ? "true" : "false"));
        return state.retiredFg.contains(handle) ? kInvalid : original.xefgSwapChainD3D12TagFrameResource(handle, commandList, id, resource);
    }
    if (!resource || resource->type < 0 || resource->type >= XEFG_SWAPCHAIN_RES_COUNT)
    {
        if (++context->invalidResourceCalls <= 4)
            WriteLog("resource_tag_rejected", "\"frame\":"+std::to_string(id)+",\"reason\":\"invalid_resource_type\"");
        return kArgument;
    }
    const unsigned type = resource->type;
    if (context->tags.frame == id) context->tags.mask &= ~(1u << type);
    const auto attempts = ++context->resourceAttempts[type];
    const auto priorResult = context->resourceResults[type];
    // dev.43: the game's motion tag may be consumed by the fill (its copy is recorded in the game's
    // list; our filled texture is tagged at the game's Present). Every other tag, and the motion tag
    // whenever the fill is off or not ready, is forwarded without alteration.
    const auto fillMode = static_cast<xess_motion_fill::Mode>(motionMode);
    ComPtr<ID3D12GraphicsCommandList> graphics;
    if (context->motionFill && commandList && (type == XEFG_SWAPCHAIN_RES_MOTION_VECTOR || type == XEFG_SWAPCHAIN_RES_DEPTH))
        commandList->QueryInterface(IID_PPV_ARGS(&graphics));
    bool consumed{};
    if (type == XEFG_SWAPCHAIN_RES_MOTION_VECTOR && context->motionFill && !context->closing && context->initialized
        && !context->poisoned)
    {
        const char* reason{};
        consumed = context->motionFill->OnMotion(graphics.Get(), id, *resource, fillMode, reason);
        if (reason) LogMotionFill(*context, reason);
    }
    // dev.47: the game's HUD-less tag is consumed by the matcher (its resource stays valid until the
    // game's Present, where our converted texture is tagged); otherwise it is forwarded unaltered and
    // the frame is not composed while the matcher is on.
    if (type == XEFG_SWAPCHAIN_RES_HUDLESS_COLOR && context->hudlessMatch && !context->closing
        && context->initialized && !context->poisoned)
    {
        const char* reason{};
        consumed = context->hudlessMatch->OnHudless(id, *resource,
            HudlessMatching() ? xess_hudless_match::Mode::eMatch : xess_hudless_match::Mode::eGame, reason);
        if (reason) LogHudlessMatch(*context, reason);
    }
    const auto result = consumed ? XEFG_SWAPCHAIN_RESULT_SUCCESS : context->closing ? kArgument :
        (!context->initialized || context->poisoned) ? XEFG_SWAPCHAIN_RESULT_ERROR_WRONG_CALL_ORDER :
        state.api.xefgSwapChainD3D12TagFrameResource(handle, commandList, id, resource);
    context->resourceResults[type] = result;
    if (attempts <= 2 || priorResult != result)
        WriteLog("resource_tag", "\"generation\":"+std::to_string(context->generation)+",\"frame\":"+std::to_string(id)+
            ",\"type\":"+std::to_string(type)+",\"validity\":"+std::to_string(resource->validity)+
            ",\"resource\":"+std::to_string(uintptr_t(resource->pResource))+",\"command_list\":"+std::to_string(uintptr_t(commandList))+
            ",\"width\":"+std::to_string(resource->resourceSize.x)+",\"height\":"+std::to_string(resource->resourceSize.y)+
            ",\"state\":"+std::to_string(resource->incomingState)+",\"result\":"+std::to_string(result)+
            ",\"consumed\":"+(consumed ? "true" : "false"));
    if (result == XEFG_SWAPCHAIN_RESULT_SUCCESS)
    {
        // dev.37 diagnostic, opt-in; a consumed motion tag is recorded from our filled texture instead
        // (dev.47: a consumed HUD-less tag from our converted texture).
        if (!consumed) xess_capture::OnTagResource(commandList, id, *resource);
        if (type == XEFG_SWAPCHAIN_RES_DEPTH && context->motionFill)
            context->motionFill->OnDepth(graphics.Get(), id, *resource, fillMode);
        ++context->resourceAccepted[type];
        ++state.display.resourceCalls;
        if (context->tags.frame != id) context->tags.Reset(id);
        context->tags.mask &= ~(1u << type);
        bool valid = resource->resourceSize.x && resource->resourceSize.y;
        if (resource->type != XEFG_SWAPCHAIN_RES_BACKBUFFER)
        {
            ComPtr<ID3D12Device> owner;
            valid = valid && resource->pResource && resource->pResource->GetDevice(IID_PPV_ARGS(&owner)) == S_OK
                && SameDevice(context->device.Get(), owner.Get());
            if (valid)
            {
                const auto description = resource->pResource->GetDesc();
                valid = description.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D && description.SampleDesc.Count == 1
                    && uint64_t(resource->resourceBase.x)+resource->resourceSize.x <= description.Width
                    && uint64_t(resource->resourceBase.y)+resource->resourceSize.y <= description.Height;
            }
        }
        if (valid)
        {
            context->tags.mask |= 1u << type;
            context->tags.width[type] = resource->resourceSize.x;
            context->tags.height[type] = resource->resourceSize.y;
            context->tags.x[type] = resource->resourceBase.x;
            context->tags.y[type] = resource->resourceBase.y;
        }
        // dev.45: composition is decided at the game's Present, with all of the frame's tags.
    }
    if (result < 0) Report(state, *context, result, "resource tag");
    return result;
}

xefg_swapchain_result_t Hook_xefgSwapChainD3D12GetSwapChainPtr(xefg_swapchain_handle_t handle, REFIID iid, void** output)
{
    auto& state = Get(); std::lock_guard lock(state.fgMutex);
    auto* context = FindFg(state, handle);
    if (!context && !state.retiredFg.contains(handle))
        return original.xefgSwapChainD3D12GetSwapChainPtr(handle, iid, output);
    if (output) *output = nullptr;
    if (!context) return kInvalid;
    if (!output) return kArgument;
    if (context->closing) return kInvalid;
    // Native QI can return its proxy even when Init never created the inner
    // swap chain. The game uses a nonnull output despite a failed Init result.
    if (!context->initialized || context->poisoned)
        return context->initializationFailure < 0
            ? static_cast<xefg_swapchain_result_t>(context->initializationFailure)
            : !context->initialized ? XEFG_SWAPCHAIN_RESULT_ERROR_UNINITIALIZED
            : XEFG_SWAPCHAIN_RESULT_ERROR_WRONG_CALL_ORDER;
    const auto result = state.api.xefgSwapChainD3D12GetSwapChainPtr(handle, iid, output);
    if (result == XEFG_SWAPCHAIN_RESULT_SUCCESS && output && *output && !context->chain)
        static_cast<IUnknown*>(*output)->QueryInterface(IID_PPV_ARGS(&context->chain));
    if (!context->poisoned && result == XEFG_SWAPCHAIN_RESULT_SUCCESS && output && *output
        && context->chain && context->presentationQueue && !context->uiAttached)
    {
        context->uiAttached = single_overlay::dx12::AttachProviderSwapchain(context->chain.Get(),
            context->presentationQueue.Get(), state.fgModule,
            context->generation, context->uiProxy.GetAddressOf());
        WriteLog("provider_ui_attachment", "\"attached\":"+std::string(context->uiAttached ? "true" : "false")+
            ",\"generation\":"+std::to_string(context->generation));
    }
    if (result == XEFG_SWAPCHAIN_RESULT_SUCCESS && output && *output && context->uiProxy)
    {
        ComPtr<IUnknown> returnedIdentity, expectedIdentity;
        static_cast<IUnknown*>(*output)->QueryInterface(IID_PPV_ARGS(&returnedIdentity));
        context->chain.As(&expectedIdentity);
        void* wrapped = nullptr;
        if (returnedIdentity && returnedIdentity == expectedIdentity
            && context->uiProxy->QueryInterface(iid, &wrapped) == S_OK && wrapped)
        {
            static_cast<IUnknown*>(*output)->Release();
            *output = wrapped;
        }
    }
    Report(state, *context, result, "swap-chain pointer");
    return result;
}

xefg_swapchain_result_t Hook_xefgSwapChainGetLastPresentStatus(xefg_swapchain_handle_t handle, xefg_swapchain_present_status_t* output)
{
    auto& state = Get(); std::lock_guard lock(state.fgMutex);
    auto* context = FindFg(state, handle);
    if (!context) return state.retiredFg.contains(handle) ? kInvalid : original.xefgSwapChainGetLastPresentStatus(handle, output);
    if (context->closing) return kInvalid;
    const auto result = state.api.xefgSwapChainGetLastPresentStatus(handle, output);
    if (result == XEFG_SWAPCHAIN_RESULT_SUCCESS && output)
    {
        const auto tick = GetTickCount64();
        const bool changed = !context->presentStatusAvailable
            || output->frameGenResult != context->presentStatus.frameGenResult
            || output->framesPresented != context->presentStatus.framesPresented
            || output->isFrameGenEnabled != context->presentStatus.isFrameGenEnabled;
        context->presentStatus = *output;
        context->presentStatusAvailable = true;
        context->presentStatusTick = tick;
        if (changed || tick - context->presentStatusLogTick >= 2000)
        {
            context->presentStatusLogTick = tick;
            WriteLog("present_status", "\"frame\":"+std::to_string(state.display.lastPresentId)+
                ",\"frames_presented\":"+std::to_string(output->framesPresented)+",\"fg_result\":"+
                std::to_string(output->frameGenResult)+",\"enabled\":"+std::to_string(output->isFrameGenEnabled)+
                ",\"hudless_attempts\":"+std::to_string(context->resourceAttempts[0])+
                ",\"depth_attempts\":"+std::to_string(context->resourceAttempts[1])+
                ",\"motion_attempts\":"+std::to_string(context->resourceAttempts[2])+
                ",\"hudless_accepted\":"+std::to_string(context->resourceAccepted[0])+
                ",\"depth_accepted\":"+std::to_string(context->resourceAccepted[1])+
                ",\"motion_accepted\":"+std::to_string(context->resourceAccepted[2]));
        }
    }
    Report(state, *context, result, "present status");
    return result;
}

xefg_swapchain_result_t Hook_xefgSwapChainSetLoggingCallback(xefg_swapchain_handle_t handle,
    xefg_swapchain_logging_level_t level, xefg_swapchain_app_log_callback_t callback, void* user)
{
    auto& state = Get(); std::lock_guard lock(state.fgMutex);
    auto* context = FindFg(state, handle);
    if (!context) return state.retiredFg.contains(handle) ? kInvalid : original.xefgSwapChainSetLoggingCallback(handle, level, callback, user);
    if (context->closing) return kInvalid;
    context->callback.store(std::make_shared<Callback>(Callback{callback, user}), std::memory_order_release);
    return state.api.xefgSwapChainSetLoggingCallback(handle, level, FgLog, context);
}

// dev.39: OptiScaler's XeLL output hands XeFG its own wrapper (InputXeLL::_xell_input_handle_t): the
// libxell handle's header (magic, impl), which XeFG reads directly, copied from the real context, then
// that real libxell handle. A wrapper pairs with the tracked context it names only when the header
// matches the context's own; both are read with ReadProcessMemory, so a stale pointer only fails the match.
template<class Map>
typename Map::iterator FindLatencyContext(Map& map, void* latency, bool& wrapped)
{
    wrapped = false;
    auto found = map.find(static_cast<xell_context_handle_t>(latency));
    if (found != map.end() || !latency) return found;
    uint64_t wrapper[3]{};
    SIZE_T got{};
    if (!ReadProcessMemory(GetCurrentProcess(), latency, wrapper, sizeof(wrapper), &got) || got != sizeof(wrapper))
        return map.end();
    found = map.find(reinterpret_cast<xell_context_handle_t>(static_cast<uintptr_t>(wrapper[2])));
    if (found == map.end()) return found;
    uint64_t header[2]{};
    if (!ReadProcessMemory(GetCurrentProcess(), found->first, header, sizeof(header), &got) || got != sizeof(header)
        || header[0] != wrapper[0] || header[1] != wrapper[1]) return map.end();
    wrapped = true;
    return found;
}
void LogLatencyRejected(const FgContext& context, void* latency, const char* reason)
{
    WriteLog("latency_pairing_rejected", "\"reason\":\""+std::string(reason)+"\",\"fg_handle\":"+
        std::to_string(uintptr_t(context.handle))+",\"xell_handle\":"+std::to_string(uintptr_t(latency))+
        ",\"generation\":"+std::to_string(context.generation));
}

// beta.8 923 (Crimson Desert): under OptiScaler's XeLL input every libxell entry jumps into OptiScaler, so the
// game's XeLL contexts never reach Hook_xellD3D12CreateContext and none is tracked. The game holds OptiScaler's
// wrapper, whose real handle OptiScaler's own SetLatencyReduction hook (ahead of ours) attaches. A tracked XeFG
// context still pairs with such a handle when it is a context of the provider's libxell: libxell's header (the
// 'XeFG' magic its handle lookup checks, then the implementation) with the implementation's vtable in that
// image's read-only data. Its device is not known here, and its lifetime stays OptiScaler's.
bool ProviderLatencyHeader(HMODULE module, const void* handle, uint64_t (&header)[2]) noexcept
{
    constexpr uint32_t kLibxellMagic = 0x47466558;
    SIZE_T got{};
    uintptr_t vtable{};
    MEMORY_BASIC_INFORMATION memory{};
    return module && handle
        && ReadProcessMemory(GetCurrentProcess(), handle, header, sizeof(header), &got) && got == sizeof(header)
        && static_cast<uint32_t>(header[0]) == kLibxellMagic && header[1]
        && ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void*>(header[1]), &vtable, sizeof(vtable), &got)
        && got == sizeof(vtable)
        && VirtualQuery(reinterpret_cast<const void*>(vtable), &memory, sizeof(memory)) == sizeof(memory)
        && memory.AllocationBase == module && memory.Type == MEM_IMAGE && memory.State == MEM_COMMIT
        && memory.Protect == PAGE_READONLY;
}
// OptiScaler's wrapper (the header copied from its real handle, then that handle) or a direct libxell handle.
// The wrapper itself carries the header, so it is tried first.
bool HostLatencyContext(HMODULE module, void* latency, bool& wrapped) noexcept
{
    wrapped = false;
    uint64_t wrapper[3]{}, header[2]{};
    SIZE_T got{};
    if (latency && ReadProcessMemory(GetCurrentProcess(), latency, wrapper, sizeof(wrapper), &got) && got == sizeof(wrapper)
        && wrapper[2] && ProviderLatencyHeader(module, reinterpret_cast<const void*>(static_cast<uintptr_t>(wrapper[2])), header)
        && header[0] == wrapper[0] && header[1] == wrapper[1])
        return wrapped = true;
    return ProviderLatencyHeader(module, latency, header);
}
xefg_swapchain_result_t PairHostLatency(State& state, FgContext& context, void* latency)
{
    bool wrapped{};
    if (!state.loaded || state.source != xess_runtime::Source::eGameCopy || !HostLatencyContext(state.llModule, latency, wrapped))
    {
        LogLatencyRejected(context, latency, "host_xell_unverified");
        return XEFG_SWAPCHAIN_RESULT_ERROR_LATENCY_REDUCTION_UNSUPPORTED;
    }
    // XeFG gets the caller's handle unchanged, as for a tracked one.
    const auto result = state.api.xefgSwapChainSetLatencyReduction(context.handle, latency);
    if (result == XEFG_SWAPCHAIN_RESULT_SUCCESS) context.hostLatency = true;
    WriteLog("latency_pairing", "\"result\":"+std::to_string(result)+",\"fg_handle\":"+
        std::to_string(uintptr_t(context.handle))+",\"xell_handle\":"+std::to_string(uintptr_t(latency))+
        ",\"host_xell\":true,\"wrapped\":"+(wrapped ? "true" : "false")+",\"generation\":"+std::to_string(context.generation));
    Report(state, context, result, "latency pairing");
    return result;
}

xefg_swapchain_result_t Hook_xefgSwapChainSetLatencyReduction(xefg_swapchain_handle_t handle, void* latency)
{
    auto& state = Get(); std::lock_guard fgLock(state.fgMutex);
    auto* context = FindFg(state, handle);
    if (!context) {
        std::lock_guard llLock(state.llMutex);
        if (state.retiredFg.contains(handle) || state.retiredLl.contains(static_cast<xell_context_handle_t>(latency))) return kInvalid;
        if (state.latency.contains(static_cast<xell_context_handle_t>(latency))) return XEFG_SWAPCHAIN_RESULT_ERROR_LATENCY_REDUCTION_UNSUPPORTED;
        return original.xefgSwapChainSetLatencyReduction(handle, latency);
    }
    if (context->closing || context->latency || context->hostLatency) return XEFG_SWAPCHAIN_RESULT_ERROR_WRONG_CALL_ORDER;
    std::lock_guard llLock(state.llMutex);
    bool wrapped{};
    const auto it = FindLatencyContext(state.latency, latency, wrapped);
    if (it == state.latency.end() && HostOwnsLatency()) return PairHostLatency(state, *context, latency);
    if (it == state.latency.end() || !SameDevice(context->device.Get(), it->second.device.Get()))
    {
        LogLatencyRejected(*context, latency, it == state.latency.end() ? "unknown_handle" : "device_mismatch");
        return XEFG_SWAPCHAIN_RESULT_ERROR_LATENCY_REDUCTION_UNSUPPORTED;
    }
    // XeFG gets the caller's handle unchanged (a wrapper reads through to the same context).
    const auto result = state.api.xefgSwapChainSetLatencyReduction(handle, latency);
    if (result == XEFG_SWAPCHAIN_RESULT_SUCCESS) { context->latency = it->first; ++it->second.references; }
    WriteLog("latency_pairing", "\"result\":"+std::to_string(result)+",\"fg_handle\":"+
        std::to_string(uintptr_t(handle))+",\"xell_handle\":"+std::to_string(uintptr_t(latency))+
        ",\"tracked_xell\":"+std::to_string(uintptr_t(it->first))+",\"wrapped\":"+(wrapped ? "true" : "false")+
        ",\"generation\":"+std::to_string(context->generation));
    Report(state, *context, result, "latency pairing");
    return result;
}

xefg_swapchain_result_t Hook_xefgSwapChainDestroy(xefg_swapchain_handle_t handle)
{
    if (tHostCall && HostOwnedHandle(handle, true)) return HostDestroyFg(Get(), handle);
    auto& state = Get(); std::lock_guard fgLock(state.fgMutex);
    auto* context = FindFg(state, handle);
    if (!context)
    {
        if (state.retiredFg.contains(handle)) return kInvalid;
        return original.xefgSwapChainDestroy(handle);
    }
    if (context->closing) return kInvalid;
    context->closing = true;
    ResetPacing(*context);
    Publish(state, context);
    if (context->uiAttached && !single_overlay::dx12::DetachProviderSwapchain(context->uiProxy.Get(), context->generation))
    {
        context->closing = false;
        Failure(state, "XeSS UI still owns in-flight resources; teardown retained.");
        return XEFG_SWAPCHAIN_RESULT_ERROR_WRONG_CALL_ORDER;
    }
    context->uiAttached = false;
    context->uiProxy.Reset();
    context->chain.Reset(); // Release our explicit observation reference before SDK teardown.
    const auto result = state.api.xefgSwapChainDestroy(handle);
    WriteLog("context_destroy", "\"result\":"+std::to_string(result)+",\"generation\":"+std::to_string(context->generation));
    if (result != XEFG_SWAPCHAIN_RESULT_SUCCESS)
    {
        context->closing = false;
        // The UI generation and our swapchain reference have already retired.
        // A failed provider teardown cannot resume the former live route.
        // Retain the exact handle/latency ownership for an explicit destroy
        // retry, but quarantine frame submission and preserve this failure
        // across otherwise successful status getters.
        context->poisoned = true;
        context->presentStatusAvailable = false;
        context->destructionFailure = result < 0 ? result : XEFG_SWAPCHAIN_RESULT_ERROR_UNKNOWN;
        Report(state, *context, result, "destruction");
        return result;
    }
    if (context->latency)
    {
        std::lock_guard llLock(state.llMutex);
        const auto it = state.latency.find(context->latency);
        if (it != state.latency.end() && it->second.references) --it->second.references;
    }
    sdk_pacing::Unregister(handle, context->generation);
    state.contexts.erase(handle);
    state.retiredFg.insert(handle);
    RestoreIdlePort(state);
    state.display.initialized = false;
    state.display.enabled = false;
    state.display.capacity = 0;
    state.display.applied = 0;
    state.display.composition = false;
    state.display.status = "XeSS context released; waiting for the game.";
    state.display.lastResult = result;
    Publish(state);
    return result;
}

#include "universal_api.inc"

// dev.40: XeLL contexts OptiScaler's own code creates (its XeLL output for its XeFG) stay
// untracked too, so no pacing reads attach to them.
xell_result_t HostCreateLl(State& state, ID3D12Device* device, xell_context_handle_t* output)
{
    const auto result = original.xellD3D12CreateContext(device, output);
    std::lock_guard lock(state.llMutex);
    if (result == XELL_RESULT_SUCCESS && *output && !state.latency.contains(*output))
    {
        state.hostLl.insert(*output);
        state.retiredLl.erase(*output);
    }
    WriteLog("host_xell_create", "\"result\":"+std::to_string(result)+",\"handle\":"+std::to_string(uintptr_t(*output)));
    return result;
}

xell_result_t Hook_xellD3D12CreateContext(ID3D12Device* device, xell_context_handle_t* output)
{
    if (!device || !output) return XELL_RESULT_ERROR_INVALID_ARGUMENT;
    *output = nullptr;
    auto& state = Get();
    if (tHostCall) return HostCreateLl(state, device, output);
    try
    {
        // Without a profiled runtime the context stays with the caller's provider, untracked.
        try { VerifyAdapter(device); }
        catch (...) { return original.xellD3D12CreateContext(device, output); }
        if (!EnsureRuntime(state)) return original.xellD3D12CreateContext(device, output);
        std::lock_guard lock(state.llMutex);
        if (state.uncertainLlLifetime || state.latency.size() >= 8) return XELL_RESULT_ERROR_INVALID_ARGUMENT;
        xell_context_handle_t created{};
        state.uncertainLlLifetime = true;
        const auto result = state.api.xellD3D12CreateContext(device, &created);
        if (result == XELL_RESULT_SUCCESS && created)
        {
            LlContext record; record.handle = created; record.device = device;
            if (!state.latency.emplace(created, std::move(record)).second) return XELL_RESULT_ERROR_UNKNOWN;
            state.retiredLl.erase(created);
            state.hostLl.erase(created); // a reused value of a destroyed host context
            state.uncertainLlLifetime = false;
            *output = created;
        }
        else if (created && !state.latency.contains(created))
            state.uncertainLlLifetime = state.api.xellDestroyContext(created) != XELL_RESULT_SUCCESS;
        else if (!created && result != XELL_RESULT_SUCCESS) state.uncertainLlLifetime = false;
        WriteLog("xell_create", "\"result\":"+std::to_string(result)+",\"handle\":"+std::to_string(uintptr_t(*output)));
        return result == XELL_RESULT_SUCCESS && !*output ? XELL_RESULT_ERROR_INVALID_CONTEXT : result;
    }
    catch (const std::exception& error)
    {
        WriteLog("xell_failure", "\"reason\":\""+escape(error.what())+"\"");
        return XELL_RESULT_ERROR_UNKNOWN;
    }
}

xell_result_t Hook_xellDestroyContext(xell_context_handle_t handle)
{
    auto& state = Get(); std::lock_guard lock(state.llMutex);
    const auto it = state.latency.find(handle);
    if (it == state.latency.end())
    {
        if (state.retiredLl.contains(handle)) return XELL_RESULT_ERROR_INVALID_CONTEXT;
        return original.xellDestroyContext(handle);
    }
    if (it->second.references || it->second.activeCalls) return XELL_RESULT_ERROR_INVALID_ARGUMENT;
    const auto result = state.api.xellDestroyContext(handle);
    if (result == XELL_RESULT_SUCCESS) { state.latency.erase(it); state.retiredLl.insert(handle); }
    WriteLog("xell_destroy", "\"result\":"+std::to_string(result));
    return result;
}

xell_result_t Hook_xellSetLoggingCallback(xell_context_handle_t handle, xell_logging_level_t level, xell_app_log_callback_t callback)
{
    auto& state = Get(); LlLease lease(state, handle);
    if (lease.retired) return XELL_RESULT_ERROR_INVALID_CONTEXT;
    return LatencyApi(state, lease).xellSetLoggingCallback(handle, level, callback);
}
// XeLL calls pass through unchanged: XeSS-FG pacing reads the frame reports XeLL builds from them.
xell_result_t Hook_xellSetSleepMode(xell_context_handle_t handle, const xell_sleep_params_t* params)
{
    auto& state = Get(); LlLease lease(state, handle);
    if (lease.retired) return XELL_RESULT_ERROR_INVALID_CONTEXT;
    return LatencyApi(state, lease).xellSetSleepMode(handle, params);
}
xell_result_t Hook_xellAddMarkerData(xell_context_handle_t handle, uint32_t frame, xell_latency_marker_type_t marker)
{
    auto& state = Get(); LlLease lease(state, handle);
    if (lease.retired) return XELL_RESULT_ERROR_INVALID_CONTEXT;
    return LatencyApi(state, lease).xellAddMarkerData(handle, frame, marker);
}
xell_result_t Hook_xellSleep(xell_context_handle_t handle, uint32_t frame)
{
    auto& state = Get(); LlLease lease(state, handle);
    if (lease.retired) return XELL_RESULT_ERROR_INVALID_CONTEXT;
    return LatencyApi(state, lease).xellSleep(handle, frame);
}
}
