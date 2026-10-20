#include "sdk_pacing.h"
#include "sdk_pacing_dlssg.h"
#include "xess_policy.h"
#include "xess_mod.h"
#include "overlay_device_identity.h"
#include "overlay_native.h"
#include "port/port_support.h"
#include "third_party/minhook/include/MinHook.h"
#include <intrin.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <dxgi1_4.h>
#include <memory>
#include <mutex>

namespace sdk_pacing
{
namespace
{
// Published once under installMutex, before the pinned provider's hooks are enabled.
uintptr_t kNativePresent=0x21f730, kPace=0x21ee30, kExtraReturn=0x2202ed, kFirstReturn=0x220db2, kRealReturn=0x220467;
uintptr_t kDeadlineReturn=0x21ef96, kDeadline=0x224b30;
uintptr_t kImplementationTable=0x157c3a0, kChainTable=0x157a980, kGetter=0xf410;
// xefg134.1: the family's object fields (runtime_layout.h Fields), published with the RVAs.
uintptr_t kImplementationChain=8, kChainQueue=0x28, kChainNative=0x30, kChainPresenter=0x120;
// The profiled presenter prologue establishes rbp = caller-rsp + 0x89.
// At the intercepted call the return slot is caller-rsp - 8, hence:
constexpr uintptr_t kSummaryFromReturn = 0x48; // [rbp - 0x49]
constexpr uintptr_t kBundleFromReturn = 0xf8;  // [rbp + 0x67]
using Present = HRESULT(*)(void*, UINT, UINT, uint64_t, const DXGI_PRESENT_PARAMETERS*, ID3D12Resource* const*, bool);
using Pace = bool(*)(void*, const void*, uint8_t, const void*, uint32_t);
using Deadline = int64_t*(*)(void*, int64_t*, const void*, const void*, uint32_t, uint32_t);

struct Owner
{
    ~Owner()
    {
        // A queue that no longer proves its table is quarantined (never released), as beta.1 did
        // with every SDK-owned interface it kept.
        if (flipQueue && !single_overlay::native::PinInterface(flipQueue.Get(), 2)) flipQueue.Detach();
    }
    void* handle{};
    uint64_t generation{};
    uintptr_t implementation{}, chain{}, presenter{}, native{}, device{};
    std::atomic<unsigned> pacedLogs{}, skippedLogs{};
    std::atomic<bool> disabled{}, retired{};
    std::mutex clockMutex;
    // DLSS-G CPU pacer interval for the SDK's own pacing (all counts).
    cadence::BatchIntervalEstimator batchInterval;
    std::atomic<unsigned> intervalLogs{};
    // hitch.1: batch-start hitches and estimator resets for the log (clockMutex).
    struct Hitches
    {
        uint64_t batches{}, hitches{}, bridged{}, resetGap{}, resetRange{}, resetBase{}, revoked{};
        unsigned hitchLogs{}, resetLogs{};
    } hitches;
    // flipmeter.1: the SDK's presentation queue (presenter+0x10 = chain+kChainQueue, the queue its native
    // swap chain presents on); null keeps this owner on plain pacing. The configuration in effect is
    // written by the presenting thread before a native Present and read by its deadlines (flipMutex).
    Microsoft::WRL::ComPtr<ID3D12CommandQueue> flipQueue;
    std::mutex flipMutex;
    struct Flip
    {
        bool configured{}, metered{}, refused{};
        uint32_t outputs{}, feedback{}, consecutiveFailures{};
        int64_t spacing{}, batchNs{};
        int status{};
        uint64_t calls{}, limited{};
        // seam.1: output 0's Present call of the batch (its frame's low 32 bits), the base of the
        // batch's metered deadlines. The SDK stamps its batch start only after waiting for output 0's
        // present fence, so deadlines from that stamp put G1 -> G2 a fence latency late and the
        // real -> G1 seam that much short (PresentMon, Subnautica 2: 4x 4.1 / 13.5 / 7.2 / 7.9 ms).
        uint32_t anchorFrame{};
        int64_t anchorNs{};
    } flip;
    std::atomic<unsigned> flipLogs{}, flipSkipLogs{};
};
using Owners = std::vector<std::shared_ptr<Owner>>;
struct Route
{
    HMODULE module{};
    Present original{};
    Pace pace{};
    Deadline deadline{};
    std::atomic<bool> enabled{};
    std::atomic<std::shared_ptr<const Owners>> owners;
    std::mutex ownersMutex;
};
std::atomic<Route*> route{};
std::mutex installMutex;

// flipmeter.1 (SetFlipMetering). firstLayout: Install verified the profiled first-output caller
// (0x220c70), whose frame First() reads for the batch's output count.
std::atomic<SetFlipConfigFunction> flipFunction{};
std::atomic<bool> flipEnabled{}, firstLayout{}, flipToggleDown{}, flipToggleKey{};
std::atomic<int64_t> flipLeadNs{2500000};
std::atomic<unsigned> flipToggleLogs{};
// A batch is configured once the estimate holds this many batch deltas (DLSS-G averages six).
constexpr uint32_t kFlipMinimumSamples = 3;
// The driver refusing this many configurations in a row turns the owner's flip metering off.
constexpr uint32_t kFlipFailureLimit = 8;
// hitch.1: the batch time the driver gets grows at most 1/kFlipGrowthDivisor per batch over the one in
// effect (same output count): every flip of the batch is held to it, so one high estimate must not
// slow a whole batch down at once. Shorter times apply at once.
constexpr int64_t kFlipGrowthDivisor = 8;
// hitch.1 log: a batch-start delta of at least kHitchRatio x the window median is a hitch.
constexpr int64_t kHitchRatio = 2;
constexpr unsigned kHitchLogLimit = 64, kResetLogLimit = 64;
// Every this many observed batches: pacing_stats.
constexpr uint64_t kHitchStatsBatches = 1800;

// NV_FLIP_CONFIG_V2 (Streamline 2.14.1 external/nvapi/nvapi.h); version = MAKE_NVAPI_VERSION(V2, 2).
struct NvFlipConfigV2
{
    UINT version, nFramesPerBatch, flipConfigFeedbackMask, nPresentedFramesPerBatch;
    float timePerBatchMs, minControlFrameTimeMs;
};
static_assert(sizeof(NvFlipConfigV2) == 24);
constexpr UINT kFlipConfigVersion2 = UINT(sizeof(NvFlipConfigV2)) | (2u << 16);
constexpr UINT kFlipFeedbackNoIflip = 1u; // NVAPI_FLIP_CONFIG_FEEDBACK_NO_IFLIP
constexpr UINT kMaximumFramesPerFlipBatch = 8; // NVAPI_MAX_FRAMES_PER_FLIP_BATCH

bool Read(uintptr_t address, void* output, size_t bytes) noexcept
{
    if (address < 0x10000 || !output || !bytes || bytes > 512
        || address > UINTPTR_MAX-bytes) return false;
    SIZE_T copied{};
    return ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void*>(address), output, bytes, &copied)
        && copied == bytes;
}
template<class T> bool Value(uintptr_t address, T& output) noexcept
{
    return Read(address, &output, sizeof(output));
}

bool MatchesApplicationDevice(ID3D12Device* device, uintptr_t presenterDevice) noexcept
{
    if (!device || !presenterDevice) return false;
    if (presenterDevice == uintptr_t(device)) return true;
    // A Streamline queue can make XeSS retain the native device while the
    // application supplied its proxy at CreateContext. Resolve only the
    // caller-owned interface; never invoke an unproven pointer read from SDK
    // memory. The presenter's field must still equal that exact native device.
    Microsoft::WRL::ComPtr<IUnknown> identity;
    Microsoft::WRL::ComPtr<ID3D12Device> native;
    return single_overlay::dx12::DeviceIdentity(device, identity)
        && SUCCEEDED(identity.As(&native)) && native
        && presenterDevice == uintptr_t(native.Get());
}
template<class T, size_t N> T At(const std::array<unsigned char, N>& bytes, size_t offset) noexcept
{
    T value{};
    if (offset <= N && sizeof(T) <= N-offset) memcpy(&value, bytes.data()+offset, sizeof(T));
    return value;
}
bool StackRange(uintptr_t address, size_t bytes) noexcept
{
    const auto* tib = reinterpret_cast<const NT_TIB*>(NtCurrentTeb());
    return address >= uintptr_t(tib->StackLimit) && address <= uintptr_t(tib->StackBase)
        && bytes <= uintptr_t(tib->StackBase)-address;
}
bool ImageRange(const unsigned char* base, uintptr_t rva, size_t bytes, DWORD* protection = nullptr) noexcept
{
    MEMORY_BASIC_INFORMATION info{};
    if (VirtualQuery(base+rva, &info, sizeof(info)) != sizeof(info) || info.AllocationBase != base
        || info.Type != MEM_IMAGE || info.State != MEM_COMMIT || info.Protect != PAGE_EXECUTE_READ
        || uintptr_t(base+rva) > uintptr_t(info.BaseAddress)+info.RegionSize
        || bytes > uintptr_t(info.BaseAddress)+info.RegionSize-uintptr_t(base+rva)) return false;
    if (protection) *protection = info.Protect;
    return true;
}

bool ContextParts(const Route& current, void* handle, uintptr_t& implementation, uintptr_t& chain) noexcept
{
    uint32_t signature{}; uintptr_t table{}, getter{};
    const auto base = uintptr_t(current.module);
    return Value(uintptr_t(handle), signature) && signature == 0x47466558
        && Value(uintptr_t(handle)+8, implementation)
        && Value(implementation, table) && table == base+kImplementationTable
        && Value(table+0x80, getter) && getter == base+kGetter
        && Value(implementation+kImplementationChain, chain)
        && Value(chain, table) && table == base+kChainTable;
}

struct Invocation
{
    void* presenter{};
    UINT sync{}, flags{};
    uint64_t frame{};
    const DXGI_PRESENT_PARAMETERS* parameters{};
    ID3D12Resource* const* resource{};
    bool generated{};
    uintptr_t returnAddress{}, returnSlot{};
};
struct Extra
{
    const void* bundle{};
    const void* summary{};
    uint32_t index{}, generatedCount{};
    uint8_t firstFence{};
    int64_t periodNs{};
};

const char* Prepare(const Route& current, const Owner& owner, const Invocation& call, Extra& output) noexcept
{
    if (owner.retired.load(std::memory_order_acquire)) return "retired_owner";
    if (call.returnAddress != uintptr_t(current.module)+kExtraReturn || !call.generated)
        return "different_call_site";
    if (call.presenter != reinterpret_cast<void*>(owner.presenter)) return "different_presenter";
    if (call.sync || (call.flags & ~UINT(DXGI_PRESENT_ALLOW_TEARING))) return "different_present_mode";
    if ((call.returnSlot & 7) || !StackRange(call.returnSlot, kBundleFromReturn+8)) return "caller_stack_bounds";
    uintptr_t savedReturn{}, bundle{}, currentPresenter{}, native{};
    if (!Value(call.returnSlot, savedReturn) || savedReturn != call.returnAddress
        || !Value(call.returnSlot+kBundleFromReturn, bundle) || !bundle
        || !Value(owner.chain+kChainPresenter, currentPresenter) || currentPresenter != owner.presenter
        || !Value(owner.presenter+0x18, native) || native != owner.native) return "ownership_changed";
    uint8_t intel{}, hardware{}, software{};
    if (!Value(owner.presenter+0x64, intel) || !Value(owner.presenter+0x340, hardware)
        || !Value(owner.presenter+0x341, software) || intel || hardware || software != 1)
        return "software_pacing_unavailable";
    std::array<unsigned char, 0xc1> data{};
    std::array<int64_t, 2> summary{};
    if (!Read(bundle, data.data(), data.size())
        || !Read(call.returnSlot+kSummaryFromReturn, summary.data(), sizeof(summary))) return "unreadable_bundle";
    const auto resources = At<uintptr_t>(data, 0);
    const auto count = At<uint64_t>(data, 8);
    const auto resourceSlot = uintptr_t(call.resource);
    if (count < 2 || count > xess_mod::kGeneratedCapacity || !resources || resourceSlot <= resources
        || resourceSlot-resources >= count*8 || ((resourceSlot-resources) & 7)) return "intermediate_index_rejected";
    if (At<UINT>(data, 0x28) != call.sync || At<UINT>(data, 0x2c) != call.flags
        || At<uint64_t>(data, 0x30) != call.frame || uintptr_t(call.parameters) != bundle+0x38)
        return "bundle_arguments_changed";
    uintptr_t resource{};
    if (!Value(resourceSlot, resource) || !resource) return "missing_output_resource";
    // This is the SDK's median nanosecond duration from at most nine history
    // entries. Startup/paused/very slow frames retain the SDK's original path.
    if (summary[0] < 1 || summary[0] > 9 || summary[1] < 1000 || summary[1] > 250000000)
        return "timing_history_unavailable";
    output = {reinterpret_cast<void*>(bundle), reinterpret_cast<void*>(call.returnSlot+kSummaryFromReturn),
        uint32_t((resourceSlot-resources)/8), uint32_t(count), uint8_t(data[0xc0] & 1), summary[1]};
    return nullptr;
}

void Revoke(Owner& owner) noexcept
{
    std::lock_guard lock(owner.clockMutex);
    owner.batchInterval.Reset();
    ++owner.hitches.revoked;
}

bool Live(const Owner& owner) noexcept
{
    if (owner.retired.load(std::memory_order_acquire) || owner.disabled.load(std::memory_order_acquire)) return false;
    uint32_t magic{}; uintptr_t implementation{}, chain{}, presenter{}, native{}, device{};
    uint8_t intel{}, hardware{}, software{};
    return Value(uintptr_t(owner.handle), magic) && magic == 0x47466558
        && Value(uintptr_t(owner.handle)+8, implementation) && implementation == owner.implementation
        && Value(implementation+kImplementationChain, chain) && chain == owner.chain
        && Value(chain+kChainPresenter, presenter) && presenter == owner.presenter
        && Value(chain+kChainNative, native) && native == owner.native
        && Value(presenter+0x18, native) && native == owner.native
        && Value(presenter, device) && device == owner.device
        && Value(presenter+0x64, intel) && !intel
        && Value(presenter+0x340, hardware) && !hardware
        && Value(presenter+0x341, software) && software == 1;
}

int64_t NowNs() noexcept
{
    LARGE_INTEGER ticks{}, frequency{};
    if (!QueryPerformanceCounter(&ticks) || !QueryPerformanceFrequency(&frequency)
        || ticks.QuadPart < 0 || frequency.QuadPart <= 0
        || frequency.QuadPart > INT64_MAX/1000000000
        || ticks.QuadPart/frequency.QuadPart > INT64_MAX/1000000000) return 0;
    return (ticks.QuadPart/frequency.QuadPart)*1000000000
        + (ticks.QuadPart%frequency.QuadPart)*1000000000/frequency.QuadPart;
}

// ---- flipmeter.1: NVIDIA driver flip metering ---------------------------------
// The batch's first generated output (beta.1 First; the profiled 0x220c70 caller, verified at
// Install): its frame holds the output vector pair, the generated count and this call's own
// arguments. Prologue: the return slot is caller-rsp - 8.
bool First(const Route& current, const Owner& owner, const Invocation& call, uint32_t& generated) noexcept
{
    if (!call.generated || call.returnAddress != uintptr_t(current.module)+kFirstReturn
        || call.presenter != reinterpret_cast<void*>(owner.presenter) || !Live(owner)
        || call.sync || (call.flags & ~UINT(DXGI_PRESENT_ALLOW_TEARING)) || (call.returnSlot & 7)
        || !StackRange(call.returnSlot, 0xf4)) return false;
    uintptr_t saved{}, pair{}, parameters{}, resources{}, resource{};
    uint64_t frame{}, count{}; uint32_t presented{}, sync{}, flags{};
    if (!Value(call.returnSlot, saved) || saved != call.returnAddress
        || !Value(call.returnSlot+0xe8, pair) || !pair
        || !Value(pair, resources) || resources != uintptr_t(call.resource)
        || !Value(pair+8, count) || !count || count > xess_mod::kGeneratedCapacity
        || count+1 > kMaximumFramesPerFlipBatch
        || !Value(call.returnSlot+0xf0, presented) || presented != 1
        || !Value(call.returnSlot+0xd8, frame) || frame != call.frame
        || !Value(call.returnSlot+0xe0, parameters) || parameters != uintptr_t(call.parameters)
        || !Value(call.returnSlot+0xc8, sync) || sync != call.sync
        || !Value(call.returnSlot+0xd0, flags) || flags != call.flags
        || !Value(resources, resource) || !resource) return false;
    generated = uint32_t(count);
    return true;
}

// The SDK's presentation queue for SetFlipConfig: the presenter's queue field equals the chain's
// (the queue XeFG created its native swap chain on), a pinned DIRECT queue, unwrapped through the
// public contracts, on the application's device. Returns the reason it is not.
const char* ResolveFlipQueue(Owner& owner, ID3D12Device* device) noexcept
{
    try
    {
        uintptr_t presenterQueue{}, chainQueue{};
        if (!Value(owner.presenter+0x10, presenterQueue) || !presenterQueue
            || !Value(owner.chain+kChainQueue, chainQueue) || chainQueue != presenterQueue) return "queue_layout";
        Microsoft::WRL::ComPtr<IUnknown> deviceIdentity, queueIdentity;
        if (!single_overlay::dx12::DeviceIdentity(device, deviceIdentity)) return "device_identity";
        auto* input = reinterpret_cast<ID3D12CommandQueue*>(presenterQueue);
        Microsoft::WRL::ComPtr<ID3D12CommandQueue> queue;
        if (!single_overlay::native::PinInterface(input, 2) || !single_overlay::native::Unwrap(input, queue)
            || !queue || !single_overlay::native::PinInterface(queue.Get(), 18)) return "queue_interface";
        ID3D12Device* raw{};
        const HRESULT result = queue->GetDevice(IID_PPV_ARGS(&raw));
        if (!raw || !single_overlay::native::PinInterface(raw, 2)) return "queue_device"; // quarantined
        Microsoft::WRL::ComPtr<ID3D12Device> queueDevice;
        queueDevice.Attach(raw);
        if (FAILED(result) || !single_overlay::dx12::DeviceIdentity(queueDevice.Get(), queueIdentity)
            || queueIdentity.Get() != deviceIdentity.Get()) return "queue_device";
        if (queue->GetDesc().Type != D3D12_COMMAND_LIST_TYPE_DIRECT) return "queue_type";
        owner.flipQueue = std::move(queue);
        return nullptr;
    }
    catch (...) { return "queue_exception"; }
}

int CallSetFlipConfig(SetFlipConfigFunction function, IUnknown* queue, NvFlipConfigV2* config) noexcept
{
    __try { return function(queue, 0, config); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return -9999; }
}

// sl.dlss_g presentFrame -> setFlipConfig: nFramesPerBatch = presented = outputs, timePerBatchMs,
// minControlFrameTimeMs 0, VidPN source 0; outputs 0 is the off form (one presented frame per
// batch, no metering). Under the owner's flipMutex, on the presenting thread.
void ConfigureFlip(Owner& owner, uint32_t outputs, int64_t batchNs, const char* reason) noexcept
{
    const auto function = flipFunction.load(std::memory_order_acquire);
    if (!function || !owner.flipQueue) return;
    auto& flip = owner.flip;
    NvFlipConfigV2 config{kFlipConfigVersion2, outputs, 0, outputs ? outputs : 1u,
        outputs && batchNs > 0 ? float(double(batchNs)/1e6) : -1.0f, 0.0f};
    const int status = CallSetFlipConfig(function, owner.flipQueue.Get(), &config);
    ++flip.calls;
    const bool statusChanged = status != flip.status, feedbackChanged = config.flipConfigFeedbackMask != flip.feedback;
    flip.status = status; flip.feedback = config.flipConfigFeedbackMask;
    if (status == 0) flip.consecutiveFailures = 0;
    else if (outputs && ++flip.consecutiveFailures >= kFlipFailureLimit) flip.refused = true;
    flip.configured = outputs && status == 0;
    flip.metered = flip.configured && !(config.flipConfigFeedbackMask & kFlipFeedbackNoIflip);
    flip.outputs = flip.configured ? outputs : 0;
    flip.batchNs = flip.configured ? batchNs : 0;
    flip.spacing = flip.configured ? batchNs/int64_t(outputs) : 0;
    if (flip.calls <= 8 || !outputs || ((statusChanged || feedbackChanged)
        && owner.flipLogs.fetch_add(1, std::memory_order_relaxed) < 32))
        try
        {
            xess_mod::WriteLog("flip_metering_config", "\"generation\":"+std::to_string(owner.generation)+
                ",\"frames_per_batch\":"+std::to_string(config.nFramesPerBatch)+
                ",\"presented_per_batch\":"+std::to_string(config.nPresentedFramesPerBatch)+
                ",\"time_per_batch_ms\":"+std::to_string(config.timePerBatchMs)+
                ",\"status\":"+std::to_string(status)+",\"feedback\":"+std::to_string(config.flipConfigFeedbackMask)+
                ",\"metered\":"+(flip.metered ? "true" : "false")+",\"refused\":"+(flip.refused ? "true" : "false")+
                ",\"calls\":"+std::to_string(flip.calls)+(reason ? ",\"reason\":\""+std::string(reason)+"\"" : ""));
        }
        catch (...) {}
}

// Wall-clock time of a log line, to line it up with other logs (OptiScaler's, the game's).
std::string LocalClock()
{
    SYSTEMTIME local{};
    GetLocalTime(&local);
    char clock[32]{};
    sprintf_s(clock, "%02u:%02u:%02u.%03u", local.wHour, local.wMinute, local.wSecond, local.wMilliseconds);
    return clock;
}

// Page Up toggles flip metering live (presenting threads, once per batch) once the user opted in
// (SetFlipToggleHotkey). flipmeter.2: was Ctrl+Shift+F8, which Subnautica 2 also binds to its in-game
// feedback screen. V1.5.1: opt-in, as Page Up is also OptiScaler's FPS overlay key.
void PollFlipToggle() noexcept
{
    if (!flipToggleKey.load(std::memory_order_acquire)) return;
    const bool down = (GetAsyncKeyState(VK_PRIOR) & 0x8000) != 0;
    if (!down) { flipToggleDown.store(false, std::memory_order_relaxed); return; }
    if (flipToggleDown.exchange(true, std::memory_order_relaxed)) return;
    const bool enabled = !flipEnabled.load(std::memory_order_acquire);
    flipEnabled.store(enabled, std::memory_order_release);
    LARGE_INTEGER qpc{};
    QueryPerformanceCounter(&qpc);
    if (flipToggleLogs.fetch_add(1, std::memory_order_relaxed) < 256)
        try
        {
            xess_mod::WriteLog("flip_metering_toggle", "\"enabled\":"+std::string(enabled ? "true" : "false")+
                ",\"local_time\":\""+LocalClock()+"\",\"now_ns\":"+std::to_string(NowNs())+",\"qpc\":"+std::to_string(qpc.QuadPart));
        }
        catch (...) {}
}

// Before the SDK's native Present: a batch's first output configures the batch, any other
// Present on the presenter (frame generation off) turns a configuration off; inner outputs and
// the real frame belong to the configured batch.
void FlipBeforePresent(Route& current, const Invocation& call)
{
    const auto offset = call.returnAddress-uintptr_t(current.module);
    const bool first = offset == kFirstReturn && call.generated;
    if (!first && (offset == kExtraReturn || offset == kRealReturn)) return;
    if (first) PollFlipToggle();
    const auto owners = current.owners.load(std::memory_order_acquire);
    if (!owners) return;
    for (const auto& entry : *owners)
    {
        auto& owner = *entry;
        if (owner.presenter != uintptr_t(call.presenter)) continue;
        if (!owner.flipQueue || owner.retired.load(std::memory_order_acquire)
            || owner.disabled.load(std::memory_order_acquire)) return;
        std::lock_guard lock(owner.flipMutex);
        auto& flip = owner.flip;
        if (!first)
        {
            if (flip.configured) ConfigureFlip(owner, 0, 0, "frame_generation_off");
            return;
        }
        const char* skip{};
        uint32_t generated{}, samples{};
        int64_t batchNs{};
        if (!flipEnabled.load(std::memory_order_acquire)) skip = "disabled";
        else if (flip.refused) skip = "driver_refused";
        else if (!firstLayout.load(std::memory_order_acquire)) skip = "first_layout";
        else if (!First(current, owner, call, generated)) skip = "first_call_layout";
        else
        {
            std::lock_guard clock(owner.clockMutex);
            batchNs = owner.batchInterval.BatchTime(generated+1);
            samples = owner.batchInterval.Samples();
            if (samples < kFlipMinimumSamples || batchNs <= 0) skip = "interval_unknown";
        }
        // hitch.1: growth over the configuration in effect for the same count is limited.
        if (!skip && flip.configured && flip.outputs == generated+1 && flip.batchNs > 0
            && batchNs > flip.batchNs+flip.batchNs/kFlipGrowthDivisor)
        {
            batchNs = flip.batchNs+flip.batchNs/kFlipGrowthDivisor;
            ++flip.limited;
        }
        if (skip)
        {
            if (owner.flipSkipLogs.fetch_add(1, std::memory_order_relaxed) < 8)
                try
                {
                    xess_mod::WriteLog("flip_metering_skipped", "\"generation\":"+std::to_string(owner.generation)+
                        ",\"frame\":"+std::to_string(call.frame)+",\"reason\":\""+skip+"\"");
                }
                catch (...) {}
            if (flip.configured) ConfigureFlip(owner, 0, 0, skip);
        }
        else ConfigureFlip(owner, generated+1, batchNs, nullptr);
        // seam.1: taken last, right before the original Present of output 0.
        flip.anchorFrame = uint32_t(call.frame);
        flip.anchorNs = NowNs();
        return;
    }
}

HRESULT Dispatch(Route& current, const Invocation& call)
{
    const DWORD incomingError = GetLastError();
    if (current.enabled.load(std::memory_order_acquire) && flipFunction.load(std::memory_order_acquire))
    {
        try { FlipBeforePresent(current, call); }
        catch (...) {} // Flip metering must not replace the original Present result.
    }
    if (current.enabled.load(std::memory_order_acquire)
        && call.returnAddress == uintptr_t(current.module)+kExtraReturn && call.generated)
    {
        try
        {
            const auto owners = current.owners.load(std::memory_order_acquire);
            if (owners) for (const auto& owner : *owners)
            {
                if (owner->presenter != uintptr_t(call.presenter) || owner->disabled.load(std::memory_order_acquire)
                    || owner->retired.load(std::memory_order_acquire)) continue;
                Extra extra{};
                if (const auto* reason = Prepare(current, *owner, call, extra))
                {
                    if (owner->skippedLogs.fetch_add(1, std::memory_order_relaxed) < 4)
                        xess_mod::WriteLog("extra_pacing_skipped", "\"generation\":"+std::to_string(owner->generation)+
                            ",\"reason\":\""+reason+"\"");
                    break;
                }
                LARGE_INTEGER before{}, after{};
                QueryPerformanceCounter(&before);
                bool paced{};
                if (owner->retired.load(std::memory_order_acquire)) break;
                try { paced = current.pace(call.presenter, extra.bundle, extra.firstFence, extra.summary, extra.index); }
                catch (...)
                {
                    owner->disabled.store(true, std::memory_order_release);
                    xess_mod::WriteLog("extra_pacing_disabled", "\"reason\":\"sdk_pacing_exception\"");
                    break;
                }
                QueryPerformanceCounter(&after);
                if (owner->pacedLogs.fetch_add(1, std::memory_order_relaxed) < 8)
                    xess_mod::WriteLog("extra_pacing", "\"generation\":"+std::to_string(owner->generation)+
                        ",\"frame\":"+std::to_string(call.frame)+",\"index\":"+std::to_string(extra.index)+
                        ",\"generated\":"+std::to_string(extra.generatedCount)+",\"median_period_ns\":"+
                        std::to_string(extra.periodNs)+",\"paced\":"+(paced ? "true" : "false")+
                        ",\"begin_qpc\":"+std::to_string(before.QuadPart)+",\"end_qpc\":"+std::to_string(after.QuadPart));
                break;
            }
        }
        catch (...) {} // The added pacing must not replace the original Present result.
    }
    SetLastError(incomingError);
    return current.original(call.presenter, call.sync, call.flags, call.frame, call.parameters, call.resource, call.generated);
}

__declspec(noinline) HRESULT HookPresent(void* presenter, UINT sync, UINT flags, uint64_t frame,
    const DXGI_PRESENT_PARAMETERS* parameters, ID3D12Resource* const* resource, bool generated)
{
    auto* current = route.load(std::memory_order_acquire);
    if (!current || !current->original) return DXGI_ERROR_INVALID_CALL;
    const Invocation call{presenter, sync, flags, frame, parameters, resource, generated,
        uintptr_t(_ReturnAddress()), uintptr_t(_AddressOfReturnAddress())};
    return Dispatch(*current, call);
}

// hitch.1: what one observed batch adds to the log. Built under the owner's clockMutex, written after
// its locks are released.
struct HitchNote
{
    bool hitch{}, reset{}, stats{};
    const char* reason{};
    int64_t delta{}, rawDelta{}, median{}, mean{}, typical{};
    uint32_t gap{}, samples{}, excluded{}, outputs{};
    Owner::Hitches counters{};
    bool Any() const noexcept { return hitch || reset || stats; }
};

void NoteBatch(Owner& owner, cadence::BatchIntervalEstimator::Event event, HitchNote& note) noexcept
{
    using Event = cadence::BatchIntervalEstimator::Event;
    if (event == Event::kRepeat) return;
    auto& h = owner.hitches;
    const auto& e = owner.batchInterval;
    ++h.batches;
    note.delta = e.LastDelta(); note.rawDelta = e.RawDelta(); note.gap = e.Gap();
    note.median = e.Median(); note.mean = e.Mean(); note.typical = e.Typical();
    note.samples = e.Samples(); note.excluded = e.Excluded(); note.outputs = e.Outputs();
    if (event == Event::kBridge) ++h.bridged;
    else if (event == Event::kResetGap) { ++h.resetGap; note.reason = "frame_gap"; }
    else if (event == Event::kResetRange) { ++h.resetRange; note.reason = "delta_range"; }
    else if (event == Event::kResetBase) { ++h.resetBase; note.reason = "no_batch_start"; }
    if (note.reason && h.resetLogs < kResetLogLimit) { ++h.resetLogs; note.reset = true; }
    if (note.delta && note.median > 0 && note.delta >= kHitchRatio*note.median)
    {
        ++h.hitches;
        if (h.hitchLogs < kHitchLogLimit) { ++h.hitchLogs; note.hitch = true; }
    }
    if (h.batches % kHitchStatsBatches == 0) { note.stats = true; note.counters = h; }
}

void LogHitches(const Owner& owner, uint32_t frame, const HitchNote& note, int64_t flipBatchNs, bool flipMetered,
    uint64_t flipLimited) noexcept
{
    try
    {
        const auto generation = ",\"generation\":"+std::to_string(owner.generation);
        const auto clock = ",\"local_time\":\""+LocalClock()+"\"";
        if (note.reset)
            xess_mod::WriteLog("pacing_reset", "\"reason\":\""+std::string(note.reason)+"\""+generation+
                ",\"frame\":"+std::to_string(frame)+",\"gap\":"+std::to_string(note.gap)+
                ",\"delta_ns\":"+std::to_string(note.rawDelta)+clock);
        if (note.hitch)
            xess_mod::WriteLog("pacing_hitch", "\"frame\":"+std::to_string(frame)+generation+
                ",\"outputs\":"+std::to_string(note.outputs)+",\"delta_ns\":"+std::to_string(note.delta)+
                ",\"median_ns\":"+std::to_string(note.median)+",\"mean_ns\":"+std::to_string(note.mean)+
                ",\"estimate_ns\":"+std::to_string(note.typical)+",\"samples\":"+std::to_string(note.samples)+
                ",\"excluded\":"+std::to_string(note.excluded)+",\"flip_time_per_batch_ns\":"+std::to_string(flipBatchNs)+
                ",\"flip_metered\":"+(flipMetered ? "true" : "false")+clock);
        if (note.stats)
        {
            const auto& c = note.counters;
            xess_mod::WriteLog("pacing_stats", "\"batches\":"+std::to_string(c.batches)+generation+
                ",\"hitches\":"+std::to_string(c.hitches)+",\"bridged\":"+std::to_string(c.bridged)+
                ",\"reset_gap\":"+std::to_string(c.resetGap)+",\"reset_range\":"+std::to_string(c.resetRange)+
                ",\"reset_base\":"+std::to_string(c.resetBase)+",\"revoked\":"+std::to_string(c.revoked)+
                ",\"estimate_ns\":"+std::to_string(note.typical)+",\"mean_ns\":"+std::to_string(note.mean)+
                ",\"median_ns\":"+std::to_string(note.median)+",\"excluded\":"+std::to_string(note.excluded)+
                ",\"outputs\":"+std::to_string(note.outputs)+",\"flip_growth_limited\":"+std::to_string(flipLimited)+clock);
        }
    }
    catch (...) {}
}

// The SDK's own Pace: replace only the arithmetic deadline with the DLSS-G interval, less the
// SDK's own present latency (latency.1).
// The SDK has already waited for output 0's present fence and stamped the base; its
// own wait helper still performs the wait.
void NativeDeadline(Route& current, void* timing, int64_t* output, const void* record,
    uint32_t index, uint32_t total) noexcept
{
    const auto owners = current.owners.load(std::memory_order_acquire);
    if (!owners) return;
    for (const auto& entry : *owners)
    {
        auto& owner = *entry;
        if (uintptr_t(timing) != owner.presenter+0x168) continue;
        uint32_t frame{}; int64_t timestamp{}, batchStart{};
        float latencyMs{};
        if (owner.disabled.load(std::memory_order_acquire) || owner.retired.load(std::memory_order_acquire)
            || !StackRange(uintptr_t(output), sizeof(*output)) || !StackRange(uintptr_t(record), 0x28)
            || !Value(uintptr_t(record), timestamp) || timestamp <= 0
            || !Value(uintptr_t(record)+0x10, frame) || !Value(uintptr_t(record)+0x14, latencyMs)
            || !Value(uintptr_t(record)+0x20, batchStart) || !Live(owner)) return;
        // latency.1: the present latency XeFG's own deadline subtracts (0x224B30 reads record+0x14, in ms, as
        // every family's witnessed body does); a value outside 0-250 ms is not used.
        const int64_t gpuLatency = std::isfinite(latencyMs) && latencyMs > 0.0f && latencyMs <= 250.0f
            ? int64_t(double(latencyMs)*1e6) : 0;
        const auto now = NowNs();
        cadence::BatchIntervalEstimator::Decision decision{};
        bool planned{};
        HitchNote note{};
        int64_t meanInterval{};
        uint32_t excluded{};
        {
            std::lock_guard lock(owner.clockMutex);
            if (index == 1) NoteBatch(owner, owner.batchInterval.Observe(frame, total, batchStart), note);
            planned = owner.batchInterval.Plan(frame, total, index, now, decision, gpuLatency);
            if (const auto outputs = owner.batchInterval.Outputs()) meanInterval = owner.batchInterval.Mean()/int64_t(outputs);
            excluded = owner.batchInterval.Excluded();
        }
        const auto sdkDeadline = *output;
        if (planned) *output = decision.deadline;
        // flipmeter.1: inside a batch the driver meters, the configured spacing (the hardware's)
        // places each output and it is released early by the lead.
        cadence::MeteredDeadline metered{};
        bool meteredApplied{}, flipMetered{};
        int64_t spacing{}, flipBatchNs{}, flipBase{};
        uint64_t flipLimited{};
        {
            std::lock_guard lock(owner.flipMutex);
            auto& flip = owner.flip;
            // seam.1: this batch's output 0 Present, when it precedes the SDK's stamp by at most one
            // batch; otherwise the stamp, as before.
            const bool anchored = flip.anchorFrame == frame && flip.anchorNs > 0 && batchStart > 0
                && flip.anchorNs <= batchStart && batchStart-flip.anchorNs <= flip.batchNs;
            flipBase = anchored ? flip.anchorNs : batchStart;
            if (flip.metered && flip.outputs == total && flipEnabled.load(std::memory_order_acquire)
                && cadence::PlanMetered(flipBase, flip.spacing, index, flipLeadNs.load(std::memory_order_relaxed),
                    now, metered))
            {
                *output = metered.deadline;
                meteredApplied = true;
                spacing = flip.spacing;
            }
            flipBatchNs = flip.batchNs; flipMetered = flip.metered; flipLimited = flip.limited;
        }
        if (note.Any()) LogHitches(owner, frame, note, flipBatchNs, flipMetered, flipLimited);
        if (owner.intervalLogs.fetch_add(1, std::memory_order_relaxed) < 24 || (index == 1 && frame%120 == 0))
            xess_mod::WriteLog("dlssg_interval_deadline", "\"generation\":"+std::to_string(owner.generation)+
                ",\"frame\":"+std::to_string(frame)+",\"index\":"+std::to_string(index)+",\"outputs\":"+std::to_string(total)+
                ",\"applied\":"+(planned ? "true" : "false")+",\"interval_ns\":"+std::to_string(decision.interval)+
                ",\"samples\":"+std::to_string(decision.samples)+",\"capped\":"+(decision.capped ? "true" : "false")+
                ",\"batch_start_ns\":"+std::to_string(batchStart)+",\"now_ns\":"+std::to_string(now)+
                ",\"sdk_deadline_ns\":"+std::to_string(sdkDeadline)+",\"deadline_ns\":"+std::to_string(*output)+
                ",\"flip_metered\":"+(meteredApplied ? "true" : "false")+",\"flip_base_ns\":"+std::to_string(flipBase)+
                ",\"flip_lead_ns\":"+std::to_string(metered.lead)+
                ",\"flip_spacing_ns\":"+std::to_string(spacing)+",\"mean_interval_ns\":"+std::to_string(meanInterval)+
                ",\"excluded\":"+std::to_string(excluded)+",\"gpu_latency_ns\":"+std::to_string(gpuLatency)+
                ",\"latency_lead_ns\":"+std::to_string(decision.lead));
        return;
    }
}

int64_t* DispatchDeadline(Route& current, void* timing, int64_t* output, const void* record,
    const void* summary, uint32_t index, uint32_t total, uintptr_t returnAddress)
{
    auto* result = current.deadline(timing, output, record, summary, index, total);
    const DWORD outgoingError = GetLastError();
    if (result == output && current.enabled.load(std::memory_order_acquire)
        && returnAddress == uintptr_t(current.module)+kDeadlineReturn
        && index >= 1 && total >= 2 && total-1 <= xess_mod::kGeneratedCapacity)
    {
        try { NativeDeadline(current, timing, output, record, index, total); }
        catch (...) {} // Logging allocation must not replace the SDK deadline.
    }
    SetLastError(outgoingError);
    return result;
}

__declspec(noinline) int64_t* HookDeadline(void* timing, int64_t* output, const void* record,
    const void* summary, uint32_t index, uint32_t total)
{
    auto* current = route.load(std::memory_order_acquire);
    if (!current || !current->deadline) return output;
    return DispatchDeadline(*current, timing, output, record, summary, index, total, uintptr_t(_ReturnAddress()));
}

// The SDK's own Pace runs unchanged; the hook provides the trampoline the extra-output
// pacing in Dispatch calls.
__declspec(noinline) bool HookPace(void* presenter, const void* bundle, uint8_t firstFence,
    const void* summary, uint32_t index)
{
    auto* current = route.load(std::memory_order_acquire);
    if (!current || !current->pace) return false;
    return current->pace(presenter, bundle, firstFence, summary, index);
}

bool HookPointsTo(unsigned char* target, uintptr_t hook) noexcept
{
    if (target[0] != 0xe9) return false;
    int32_t relative{}; memcpy(&relative, target+1, 4);
    const uintptr_t relay = uintptr_t(target+5)+relative;
    if (relay == hook) return true;
    std::array<unsigned char, 14> bytes{};
    if (!Read(relay, bytes.data(), bytes.size()) || bytes[0] != 0xff || bytes[1] != 0x25) return false;
    int32_t displacement{}; memcpy(&displacement, bytes.data()+2, 4);
    uintptr_t destination{};
    return Value(relay+6+displacement, destination) && destination == hook;
}
bool HookPointsToUs(unsigned char* target) noexcept { return HookPointsTo(target, uintptr_t(&HookPresent)); }
}

bool Install(HMODULE provider, std::string& failure) noexcept
{
    try
    {
        std::lock_guard lock(installMutex);
        failure.clear();
        if (auto* current = route.load(std::memory_order_acquire))
            return current->module == provider && current->enabled.load(std::memory_order_acquire);
        wchar_t path[32768]{};
        const DWORD length = GetModuleFileNameW(provider, path, DWORD(std::size(path)));
        if (!provider || !length || length >= std::size(path)
            || !xess_layout::Get(provider).family)
            throw std::runtime_error("pacing_provider_identity");
        auto* base = reinterpret_cast<unsigned char*>(provider);
        const auto& layout=xess_layout::Get(provider);
        for (const auto function : {0x21f730u,0x21ee30u,0x224b30u})
            layout.VerifyFunction(provider,function);
        // flipmeter.1: First() reads the first-output caller's frame; without it only flip metering is off.
        bool firstVerified{};
        try { layout.VerifyFunction(provider, 0x220c70u); firstVerified = true; }
        catch (const std::exception&) {}
        kNativePresent=layout.Rva(0x21f730); kPace=layout.Rva(0x21ee30);
        kExtraReturn=layout.Rva(0x2202ed); kFirstReturn=layout.Rva(0x220db2); kRealReturn=layout.Rva(0x220467);
        kDeadlineReturn=layout.Rva(0x21ef96); kDeadline=layout.Rva(0x224b30);
        kImplementationTable=layout.Rva(0x157c3a0); kChainTable=layout.Rva(0x157a980); kGetter=layout.Rva(0xf410);
        kImplementationChain=layout.fields.implementationChain; kChainQueue=layout.fields.chainQueue;
        kChainNative=layout.fields.chainNative; kChainPresenter=layout.fields.chainPresenter;
        for (auto module : {provider, xess_mod::Self()})
        {
            HMODULE pinned{};
            if (!module || !GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                reinterpret_cast<LPCWSTR>(module), &pinned) || pinned != module) throw std::runtime_error("pacing_module_pin");
        }
        const auto initialized = MH_Initialize();
        if (initialized != MH_OK && initialized != MH_ERROR_ALREADY_INITIALIZED)
            throw std::runtime_error("pacing_hook_initialization");
        auto* current = new Route; // Callback and trampoline have process lifetime.
        current->module = provider;
        struct Hook { uintptr_t rva; void* callback; void** original; };
        const std::array<Hook, 3> hooks{{
            {kNativePresent, reinterpret_cast<void*>(&HookPresent), reinterpret_cast<void**>(&current->original)},
            {kPace, reinterpret_cast<void*>(&HookPace), reinterpret_cast<void**>(&current->pace)},
            {kDeadline, reinterpret_cast<void*>(&HookDeadline), reinterpret_cast<void**>(&current->deadline)}}};
        size_t created{};
        for (const auto& hook : hooks)
        {
            if (MH_CreateHook(base+hook.rva, hook.callback, hook.original) != MH_OK)
            {
                while (created) MH_RemoveHook(base+hooks[--created].rva);
                delete current;
                throw std::runtime_error("pacing_hook_creation");
            }
            ++created;
        }
        route.store(current, std::memory_order_release);
        for (const auto& hook : hooks)
        {
            if (MH_EnableHook(base+hook.rva) != MH_OK || !ImageRange(base, hook.rva, 16)
                || !HookPointsTo(base+hook.rva, uintptr_t(hook.callback)))
            {
                // Keep every callback/trampoline pinned, even if restoration
                // fails. The published route stays disabled and admits no owner.
                for (const auto& rollback : hooks) MH_DisableHook(base+rollback.rva);
                throw std::runtime_error("pacing_hook_activation");
            }
        }
        firstLayout.store(firstVerified, std::memory_order_release);
        current->enabled.store(true, std::memory_order_release);
        xess_mod::WriteLog("extra_pacing_hook", "\"native_present_rva\":"+std::to_string(kNativePresent)+
            ",\"extra_return_rva\":"+std::to_string(kExtraReturn)+",\"first_return_rva\":"+std::to_string(kFirstReturn)+
            ",\"real_return_rva\":"+std::to_string(kRealReturn)+",\"deadline_hook\":true,\"profile_verified\":true"+
            ",\"first_caller_verified\":"+(firstVerified ? "true" : "false")+
            ",\"family\":\""+layout.family+"\"");
        return true;
    }
    catch (const std::exception& error) { failure = error.what(); }
    catch (...) { failure = "pacing_install_exception"; }
    return false;
}

bool Register(void* handle, uint64_t generation, ID3D12Device* device, std::string& failure) noexcept
{
    try
    {
        failure.clear();
        auto* current = route.load(std::memory_order_acquire);
        if (!current || !current->enabled.load(std::memory_order_acquire) || !handle || !generation || !device)
            throw std::runtime_error("pacing_registration_unavailable");
        auto owner = std::make_shared<Owner>();
        owner->handle = handle; owner->generation = generation;
        uintptr_t native{}, presenterDevice{};
        if (!ContextParts(*current, handle, owner->implementation, owner->chain)
            || !Value(owner->chain+kChainPresenter, owner->presenter)
            || !Value(owner->chain+kChainNative, owner->native)
            || !Value(owner->presenter+0x18, native) || native != owner->native
            || !Value(owner->presenter, presenterDevice))
            throw std::runtime_error("pacing_context_ownership");
        if (!MatchesApplicationDevice(device, presenterDevice))
            throw std::runtime_error("pacing_context_device_identity");
        owner->device = presenterDevice;
        uint8_t intel{}, hardware{}, software{};
        if (!Value(owner->presenter+0x64, intel) || !Value(owner->presenter+0x340, hardware)
            || !Value(owner->presenter+0x341, software) || intel || hardware || software != 1)
            throw std::runtime_error("pacing_context_mode");
        std::lock_guard lock(current->ownersMutex);
        const auto prior = current->owners.load(std::memory_order_acquire);
        auto next = prior ? std::make_shared<Owners>(*prior) : std::make_shared<Owners>();
        for (const auto& entry : *next)
        {
            if (entry->handle == handle && entry->generation == generation && entry->presenter == owner->presenter
                && entry->chain == owner->chain && entry->native == owner->native && entry->device == owner->device) return true;
            if (entry->handle == handle || entry->presenter == owner->presenter)
                throw std::runtime_error("pacing_owner_collision");
        }
        if (next->size() >= 8) throw std::runtime_error("pacing_owner_capacity");
        // flipmeter.1: only with a driver function; the owner keeps plain pacing without its queue.
        if (flipFunction.load(std::memory_order_acquire))
        {
            const char* reason = ResolveFlipQueue(*owner, device);
            xess_mod::WriteLog("flip_metering_queue", "\"generation\":"+std::to_string(generation)+
                ",\"resolved\":"+(reason ? "false" : "true")+(reason ? ",\"reason\":\""+std::string(reason)+"\"" : "")+
                ",\"queue\":"+std::to_string(uintptr_t(owner->flipQueue.Get()))+
                ",\"enabled\":"+(flipEnabled.load() ? "true" : "false")+",\"lead_ns\":"+std::to_string(flipLeadNs.load())+
                ",\"first_caller_verified\":"+(firstLayout.load() ? "true" : "false"));
        }
        next->push_back(owner);
        current->owners.store(next, std::memory_order_release);
        xess_mod::WriteLog("extra_pacing_owner", "\"generation\":"+std::to_string(generation)+
            ",\"handle\":"+std::to_string(uintptr_t(handle))+",\"sdk_swapchain\":"+std::to_string(owner->chain)+
            ",\"presenter\":"+std::to_string(owner->presenter)+",\"native_swapchain\":"+std::to_string(owner->native)+
            ",\"application_device\":"+std::to_string(uintptr_t(device))+
            ",\"presenter_device\":"+std::to_string(presenterDevice)+
            ",\"native_device_identity_used\":"+(presenterDevice != uintptr_t(device) ? "true" : "false"));
        return true;
    }
    catch (const std::exception& error) { failure = error.what(); }
    catch (...) { failure = "pacing_registration_exception"; }
    return false;
}

void Reset(void* handle, uint64_t generation) noexcept
{
    try
    {
        if (auto* current = route.load(std::memory_order_acquire))
            if (const auto owners = current->owners.load(std::memory_order_acquire)) for (const auto& owner : *owners)
                if (owner->handle == handle && owner->generation == generation) Revoke(*owner);
    }
    catch (...) {}
}

void Unregister(void* handle, uint64_t generation) noexcept
{
    try
    {
        auto* current = route.load(std::memory_order_acquire);
        if (!current) return;
        std::lock_guard lock(current->ownersMutex);
        const auto prior = current->owners.load(std::memory_order_acquire);
        if (!prior) return;
        auto next = std::make_shared<Owners>();
        for (const auto& entry : *prior)
            if (entry->handle != handle || entry->generation != generation) next->push_back(entry);
            // In-flight callbacks retain their Owner snapshot.
            else { entry->retired.store(true, std::memory_order_release); Revoke(*entry); }
        current->owners.store(next, std::memory_order_release);
    }
    catch (...)
    {
        // Allocation failure must still remove all possible stale identities.
        if (auto* current = route.load(std::memory_order_acquire))
        {
            if (const auto owners = current->owners.load(std::memory_order_acquire)) for (const auto& owner : *owners)
            { owner->retired.store(true, std::memory_order_release); Revoke(*owner); }
            current->owners.store({}, std::memory_order_release);
        }
    }
}

void SetFlipMetering(SetFlipConfigFunction setFlipConfig, bool enabled, int64_t innerLeadNs) noexcept
{
    flipLeadNs.store(innerLeadNs >= 0 && innerLeadNs <= 8000000 ? innerLeadNs : 2500000, std::memory_order_relaxed);
    flipEnabled.store(enabled && setFlipConfig, std::memory_order_release);
    flipFunction.store(setFlipConfig, std::memory_order_release);
}

void SetFlipToggleHotkey(bool enabled) noexcept
{
    flipToggleDown.store(false, std::memory_order_relaxed);
    flipToggleKey.store(enabled, std::memory_order_release);
}
}
