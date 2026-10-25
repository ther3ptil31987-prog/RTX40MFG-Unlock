#include "dlss_sr.h"
#include "dlss_sr_override.h"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <mutex>

namespace dlss_sr
{
namespace
{
std::mutex gMutex;
Tracker<8, 8> gTracker; // guarded by gMutex

// A saved selection per feature, loaded lazily until the worker publishes one.
struct Selection
{
    std::atomic<uint32_t> value{kGameOrDriver};
    std::atomic<bool> initialized{false};
    std::atomic<InitialSelectionLoader> loader{nullptr};
    std::mutex mutex;
};
Selection gSelections[2];

std::atomic<LogCallback> gLog{nullptr};
std::atomic<DrsHookInstaller> gInstaller{nullptr};
std::atomic<RuntimeEligibilityProbe> gRuntimeEligibility{nullptr};
std::atomic<bool> gDrsHookReady{false};

// Each provider's DRS read, guarded by gDrsMutex.
struct DrsState
{
    bool observed = false;
    bool driverValueValid = false;
    uint32_t driverValue = 0;
    bool suppressed = false;
    uint64_t reads = 0;
    uint64_t suppressedReads = 0;
};
std::mutex gDrsMutex;
DrsState gDrs[2];
std::atomic<bool> gFixedByDriver[2]{};
std::atomic<uint64_t> gDrsCalls{0};
std::atomic<uint64_t> gDrsNotProvider{0};
std::atomic<uint64_t> gDrsNotWritable{0};
std::atomic<uint64_t> gCreates{0};
std::atomic<uint64_t> gCreateRetries{0};
// Fast path for unrelated (frame-generation) Evaluate/Release traffic.
std::atomic<uint32_t> gTracked{0};
std::atomic<uint32_t> gRetiredPending{0};
std::atomic<bool> gReplayDenied{false};
bool RuntimeOverrideAllowed(Kind kind) noexcept
{
    const auto probe=gRuntimeEligibility.load(std::memory_order_acquire);
    return probe && probe() && gDrsHookReady.load(std::memory_order_acquire)
        && dlss_sr_override::Supports(kind);
}
#if defined(MFG_DLSS_SR_TESTING)
std::atomic<testing::Clock> gClock{nullptr};
std::atomic<testing::ProviderCheck> gProviderCheck{nullptr};
#endif

constexpr size_t Index(Kind kind) noexcept { return kind == Kind::eSuperResolution ? 0 : 1; }
constexpr const wchar_t* KindName(Kind kind) noexcept { return kind == Kind::eSuperResolution ? L"SR" : L"RR"; }
constexpr NVSDK_NGX_Feature FeatureOf(Kind kind) noexcept
{
    return kind == Kind::eSuperResolution ? NVSDK_NGX_Feature_SuperSampling : NVSDK_NGX_Feature_RayReconstruction;
}

uint64_t NowMs() noexcept
{
#if defined(MFG_DLSS_SR_TESTING)
    if (const auto clock = gClock.load(std::memory_order_acquire)) return clock();
#endif
    return GetTickCount64();
}

void Log(const wchar_t* text) noexcept
{
    if (const auto log = gLog.load(std::memory_order_acquire)) log(text);
}

const wchar_t* PresetName(uint32_t preset) noexcept
{
    static constexpr const wchar_t* kLetters[]{L"default", L"A", L"B", L"C", L"D", L"E", L"F", L"G",
        L"H", L"I", L"J", L"K", L"L", L"M", L"N", L"O"};
    if (preset < std::size(kLetters)) return kLetters[preset];
    return preset == kDrsLatest ? L"latest" : L"?";
}

void SetSelection(Kind kind, uint32_t preset) noexcept
{
    Selection& selection = gSelections[Index(kind)];
    std::lock_guard lock(selection.mutex);
    selection.value.store(IsValidSelectionFor(kind, preset) ? preset : kGameOrDriver, std::memory_order_release);
    selection.initialized.store(true, std::memory_order_release);
}

uint32_t GetSelection(Kind kind) noexcept
{
    Selection& selection = gSelections[Index(kind)];
    if (!selection.initialized.load(std::memory_order_acquire))
    {
        if (const auto loader = selection.loader.load(std::memory_order_acquire))
        {
            const uint32_t loaded = loader();
            std::lock_guard lock(selection.mutex);
            if (!selection.initialized.load(std::memory_order_acquire))
            {
                selection.value.store(IsValidSelectionFor(kind, loaded) ? loaded : kGameOrDriver,
                    std::memory_order_release);
                selection.initialized.store(true, std::memory_order_release);
            }
        }
    }
    return selection.value.load(std::memory_order_acquire);
}

// Called with gMutex held.
void Publish() noexcept
{
    const auto& counters = gTracker.Read();
    gTracked.store(counters.liveSr + counters.liveRr, std::memory_order_release);
    gRetiredPending.store(counters.retiredPending, std::memory_order_release);
}

// NGX parameter values are typed; games and Streamline set these keys as
// unsigned or signed int. Each read tries both and remembers which one held.
enum class Stored : uint8_t { eAbsent, eUnsigned, eSigned };

Stored GetValue(const NVSDK_NGX_Parameter* parameters, const char* name, uint32_t& value) noexcept
{
    __try
    {
        unsigned int unsignedValue = 0;
        if (parameters->Get(name, &unsignedValue) == NVSDK_NGX_Result_Success)
        {
            value = unsignedValue;
            return Stored::eUnsigned;
        }
        int signedValue = 0;
        if (parameters->Get(name, &signedValue) == NVSDK_NGX_Result_Success)
        {
            value = static_cast<uint32_t>(signedValue);
            return Stored::eSigned;
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
    return Stored::eAbsent;
}

bool SetValue(const NVSDK_NGX_Parameter* parameters, const char* name, uint32_t value, Stored as) noexcept
{
    auto* writable = const_cast<NVSDK_NGX_Parameter*>(parameters);
    __try
    {
        if (as == Stored::eSigned) writable->Set(name, static_cast<int>(value));
        else writable->Set(name, static_cast<unsigned int>(value));
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

CreateKeys ReadKeys(const NVSDK_NGX_Parameter* parameters) noexcept
{
    CreateKeys keys{};
    if (!parameters) return keys;
    const auto read = [&](const char* name, uint32_t bit, uint32_t& value) noexcept
    {
        const Stored type = GetValue(parameters, name, value);
        if (type != Stored::eAbsent) keys.present |= bit;
        if (type == Stored::eSigned) keys.signedValues |= bit;
    };
    uint32_t perfQuality = 0, createFlags = 0, outputSubrects = 0, denoiseMode = 0;
    read(NVSDK_NGX_Parameter_Width, CreateKeys::eWidth, keys.width);
    read(NVSDK_NGX_Parameter_Height, CreateKeys::eHeight, keys.height);
    read(NVSDK_NGX_Parameter_OutWidth, CreateKeys::eOutWidth, keys.outWidth);
    read(NVSDK_NGX_Parameter_OutHeight, CreateKeys::eOutHeight, keys.outHeight);
    read(NVSDK_NGX_Parameter_CreationNodeMask, CreateKeys::eCreationNodeMask, keys.creationNodeMask);
    read(NVSDK_NGX_Parameter_VisibilityNodeMask, CreateKeys::eVisibilityNodeMask, keys.visibilityNodeMask);
    read(NVSDK_NGX_Parameter_PerfQualityValue, CreateKeys::ePerfQuality, perfQuality);
    read(NVSDK_NGX_Parameter_DLSS_Feature_Create_Flags, CreateKeys::eCreateFlags, createFlags);
    read(NVSDK_NGX_Parameter_DLSS_Enable_Output_Subrects, CreateKeys::eOutputSubrects, outputSubrects);
    // NVSDK_NGX_Parameter_DLSS_Denoise_Mode / _Roughness_Mode / _Use_HW_Depth (nvsdk_ngx_defs_dlssd.h).
    read("DLSS.Denoise.Mode", CreateKeys::eDenoiseMode, denoiseMode);
    read("DLSS.Roughness.Mode", CreateKeys::eRoughnessMode, keys.roughnessMode);
    read("DLSS.Use.HW.Depth", CreateKeys::eUseHwDepth, keys.useHwDepth);
    keys.perfQuality = keys.present & CreateKeys::ePerfQuality ? static_cast<int32_t>(perfQuality) : -1;
    keys.createFlags = static_cast<int32_t>(createFlags);
    keys.outputSubrects = static_cast<int32_t>(outputSubrects);
    keys.denoiseMode = static_cast<int32_t>(denoiseMode);
    return keys;
}

uint32_t ReadGameHint(Kind kind, const NVSDK_NGX_Parameter* parameters, int32_t perfQuality) noexcept
{
    const HintKey key = HintForQuality(perfQuality);
    uint32_t value = 0;
    if (!parameters || key == HintKey::eCount
        || GetValue(parameters, HintNamesFor(kind)[static_cast<size_t>(key)], value) == Stored::eAbsent) return 0;
    return value;
}

struct SavedHints
{
    std::array<uint32_t, kHintNames.size()> value{};
    std::array<Stored, kHintNames.size()> stored{};
    const HintNames* names = nullptr;
    bool applied = false;
    bool valid = true;
};

// Streamline shares this parameter object between features and its optimal-
// settings queries. Snapshot values, never a retained/dereferenced Create pointer.
constexpr std::array<const char*, 12> kCreateNames{
    NVSDK_NGX_Parameter_Width, NVSDK_NGX_Parameter_Height,
    NVSDK_NGX_Parameter_OutWidth, NVSDK_NGX_Parameter_OutHeight,
    NVSDK_NGX_Parameter_CreationNodeMask, NVSDK_NGX_Parameter_VisibilityNodeMask,
    NVSDK_NGX_Parameter_PerfQualityValue, NVSDK_NGX_Parameter_DLSS_Feature_Create_Flags,
    NVSDK_NGX_Parameter_DLSS_Enable_Output_Subrects,
    "DLSS.Denoise.Mode", "DLSS.Roughness.Mode", "DLSS.Use.HW.Depth"};
std::array<uint32_t, 12> CreateValues(const CreateKeys& k) noexcept
{
    return {k.width,k.height,k.outWidth,k.outHeight,k.creationNodeMask,k.visibilityNodeMask,
        static_cast<uint32_t>(k.perfQuality),static_cast<uint32_t>(k.createFlags),
        static_cast<uint32_t>(k.outputSubrects),static_cast<uint32_t>(k.denoiseMode),k.roughnessMode,k.useHwDepth};
}
struct SavedCreateKeys
{
    std::array<uint32_t, 12> values{};
    std::array<Stored, 12> types{};
    uint32_t written = 0;
    bool valid = true;
};
bool RestoreCreateKeys(const NVSDK_NGX_Parameter* p,const SavedCreateKeys& saved) noexcept
{
    bool restored=true;
    for(size_t i=0;i<kCreateNames.size();++i)
        if(saved.written&(1u<<i))
        {
            uint32_t seen=0;
            const bool set=SetValue(p,kCreateNames[i],saved.values[i],saved.types[i]);
            restored &= set&&GetValue(p,kCreateNames[i],seen)!=Stored::eAbsent&&seen==saved.values[i];
        }
    return restored;
}
SavedCreateKeys ApplyCreateKeys(const NVSDK_NGX_Parameter* p,const CreateRecord& original) noexcept
{
    SavedCreateKeys saved{};
    const auto values=CreateValues(original.keys);
    const size_t count=original.kind==Kind::eSuperResolution?9:12;
    // Read everything before writing. Missing original fields are never invented;
    // later optional fields get their documented default (zero), then are restored.
    for(size_t i=0;i<count;++i)
    {
        saved.types[i]=GetValue(p,kCreateNames[i],saved.values[i]);
        if((original.keys.present&(1u<<i))&&saved.types[i]==Stored::eAbsent)saved.valid=false;
        // Only output-subrects has a replayed absent -> false default. Do not
        // guess defaults for a later-added quality, flags, node or RR mode.
        if(!(original.keys.present&(1u<<i))&&saved.types[i]!=Stored::eAbsent&&i!=8)saved.valid=false;
    }
    if(!saved.valid)return saved;
    for(size_t i=0;i<count;++i)
    {
        if(saved.types[i]==Stored::eAbsent)continue;
        const uint32_t value=(original.keys.present&(1u<<i))?values[i]:0;
        const Stored as=(original.keys.signedValues&(1u<<i))?Stored::eSigned:Stored::eUnsigned;
        saved.written|=1u<<i; // restore even if Set faults part way through
        uint32_t seen=0;
        if(!SetValue(p,kCreateNames[i],value,as)||GetValue(p,kCreateNames[i],seen)==Stored::eAbsent||seen!=value)
        {saved.valid=false;break;}
    }
    return saved;
}
void CaptureHints(CreateRecord& record,const NVSDK_NGX_Parameter* p) noexcept
{
    const auto& names=HintNamesFor(record.kind);
    for(size_t i=0;i<names.size();++i)
    {
        const auto type=GetValue(p,names[i],record.hints[i]);
        if(type!=Stored::eAbsent)record.hintsPresent|=1u<<i;
        if(type==Stored::eSigned)record.hintsSigned|=1u<<i;
    }
}
SavedHints ReplayHints(const NVSDK_NGX_Parameter* p,const CreateRecord& original) noexcept
{
    SavedHints saved{};saved.names=&HintNamesFor(original.kind);
    for(size_t i=0;i<saved.names->size();++i)
        saved.stored[i]=GetValue(p,(*saved.names)[i],saved.value[i]);
    for(size_t i=0;i<saved.names->size();++i)
    {
        const bool present=original.hintsPresent&(1u<<i);
        const uint32_t value=present?original.hints[i]:0;
        saved.applied=true;
        uint32_t seen=0;
        saved.valid &= SetValue(p,(*saved.names)[i],value,
            (original.hintsSigned&(1u<<i))?Stored::eSigned:Stored::eUnsigned)
            && GetValue(p,(*saved.names)[i],seen)!=Stored::eAbsent && seen==value;
    }
    return saved;
}

// Every quality mode gets the selection, so the game's mode needs no decoding.
SavedHints ApplyHints(Kind kind, const NVSDK_NGX_Parameter* parameters, uint32_t preset) noexcept
{
    SavedHints saved{};
    if (!parameters || !IsOverrideFor(kind, preset)) return saved;
    saved.names = &HintNamesFor(kind);
    for (size_t i = 0; i != saved.names->size(); ++i)
        saved.stored[i] = GetValue(parameters, (*saved.names)[i], saved.value[i]);
    for (size_t i = 0; i != saved.names->size(); ++i)
        saved.applied |= SetValue(parameters, (*saved.names)[i], preset, Stored::eUnsigned);
    return saved;
}

// An absent key reads as Default (0) to the provider, so 0 restores it.
bool RestoreHints(const NVSDK_NGX_Parameter* parameters, const SavedHints& saved) noexcept
{
    if (!saved.applied) return true;
    bool restored=true;
    for (size_t i = 0; i != saved.names->size(); ++i)
    {
        const bool present = saved.stored[i] != Stored::eAbsent;
        const uint32_t value=present ? saved.value[i] : 0;
        const bool set=SetValue(parameters, (*saved.names)[i], value,
            present ? saved.stored[i] : Stored::eUnsigned);
        uint32_t seen=0;
        restored &= set&&GetValue(parameters,(*saved.names)[i],seen)!=Stored::eAbsent&&seen==value;
    }
    return restored;
}

ID3D12Device* CommandDevice(ID3D12GraphicsCommandList* command) noexcept
{
    ID3D12Device* device = nullptr;
    if (!command) return nullptr;
    __try
    {
        if (FAILED(command->GetDevice(__uuidof(ID3D12Device), reinterpret_cast<void**>(&device)))) device = nullptr;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { device = nullptr; }
    return device;
}

uintptr_t CreatedHandle(NVSDK_NGX_Result result, NVSDK_NGX_Handle** output) noexcept
{
    if (result != NVSDK_NGX_Result_Success || !output) return 0;
    __try { return reinterpret_cast<uintptr_t>(*output); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

// An NGX feature provider (snippet) exports NVSDK_NGX_GetSnippetVersion; the
// NGX runtime does not. Read from the export table, never via GetProcAddress
// (this runs inside an NvAPI call).
bool ExportsName(HMODULE module, const char* name) noexcept
{
    if (!module) return false;
    __try
    {
        const auto base = reinterpret_cast<const uint8_t*>(module);
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
        if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0 || dos->e_lfanew > 0x100000) return false;
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
        if (nt->Signature != IMAGE_NT_SIGNATURE || nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) return false;
        const auto& directory = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
        const uint32_t imageSize = nt->OptionalHeader.SizeOfImage;
        if (!directory.VirtualAddress || directory.VirtualAddress >= imageSize
            || directory.Size > imageSize - directory.VirtualAddress) return false;
        const auto* exports = reinterpret_cast<const IMAGE_EXPORT_DIRECTORY*>(base + directory.VirtualAddress);
        if (exports->AddressOfNames >= imageSize
            || static_cast<uint64_t>(exports->NumberOfNames) * 4 > imageSize - exports->AddressOfNames) return false;
        const auto* names = reinterpret_cast<const uint32_t*>(base + exports->AddressOfNames);
        const size_t length = strlen(name);
        for (uint32_t i = 0; i != exports->NumberOfNames; ++i)
        {
            if (names[i] >= imageSize || length + 1 > imageSize - names[i]) continue;
            if (memcmp(base + names[i], name, length + 1) == 0) return true;
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
    return false;
}

bool IsProvider(HMODULE caller) noexcept
{
#if defined(MFG_DLSS_SR_TESTING)
    if (const auto check = gProviderCheck.load(std::memory_order_acquire)) return check(caller);
#endif
    return ExportsName(caller, "NVSDK_NGX_GetSnippetVersion");
}

const wchar_t* ApiName(CreateApi api) noexcept
{
    return api == CreateApi::eVulkan ? L"vulkan" : api == CreateApi::eVulkan1 ? L"vulkan1" : L"d3d12";
}

void LogCreate(const CreateRecord& record, NVSDK_NGX_Result result, const wchar_t* what) noexcept
{
    wchar_t text[400]{};
    swprintf_s(text, L"DLSS %ls %ls: result=0x%08X handle=%p render=%ux%u output=%ux%u quality=%d "
        L"gameHint=%ls selection=%ls fixedByDriver=%d api=%ls",
        KindName(record.kind), what, static_cast<uint32_t>(result),
        reinterpret_cast<void*>(record.handle), record.keys.width, record.keys.height,
        record.keys.outWidth, record.keys.outHeight, record.keys.perfQuality,
        PresetName(record.gameHint), record.preset ? PresetName(record.preset) : L"game/driver",
        gFixedByDriver[Index(record.kind)].load(std::memory_order_acquire), ApiName(record.api));
    Log(text);
}

// The call's own entry is present (CreateFeature1 also needs its VkDevice).
bool Callable(const CreateCall& call) noexcept
{
    switch (call.api)
    {
    case CreateApi::eVulkan: return call.vulkan != nullptr;
    case CreateApi::eVulkan1: return call.vulkan1 && call.device;
    default: return call.d3d12 != nullptr;
    }
}

NVSDK_NGX_Result Invoke(const CreateCall& call, NVSDK_NGX_Feature feature, const NVSDK_NGX_Parameter* parameters,
    NVSDK_NGX_Handle** output) noexcept
{
    if (!Callable(call)) return NVSDK_NGX_Result_FAIL_NotInitialized;
    switch (call.api)
    {
    case CreateApi::eVulkan: return call.vulkan(call.command, feature, parameters, output);
    case CreateApi::eVulkan1: return call.vulkan1(call.device, call.command, feature, parameters, output);
    default: return call.d3d12(static_cast<ID3D12GraphicsCommandList*>(call.command), feature, parameters, output);
    }
}

// A live re-create on this Evaluate's command list or buffer, through the
// entry the game's Create used (CreateFeature1 with the game's VkDevice).
// Not callable when this Evaluate belongs to another API or the route lacks it.
CreateCall RecreateCall(const CreateCall& evaluate, const CreateRecord& creation) noexcept
{
    CreateCall call{};
    call.api = creation.api;
    call.command = evaluate.command;
    const bool vulkan = evaluate.api != CreateApi::eD3D12;
    if (creation.api == CreateApi::eD3D12 && !vulkan) call.d3d12 = evaluate.d3d12;
    else if (creation.api == CreateApi::eVulkan && vulkan) call.vulkan = evaluate.vulkan;
    else if (creation.api == CreateApi::eVulkan1 && vulkan)
    {
        call.vulkan1 = evaluate.vulkan1;
        call.device = reinterpret_cast<void*>(creation.device);
    }
    return call;
}

NVSDK_NGX_Result InvokeCreate(const CreateCall& call,NVSDK_NGX_Feature feature,
    const NVSDK_NGX_Parameter* parameters,NVSDK_NGX_Handle** output,const SavedHints* saved,
    dlss_sr_override::Request* request,const SavedCreateKeys* keys = nullptr,bool* restored = nullptr) noexcept
{
    __try {return Invoke(call,feature,parameters,output);}
    __finally
    {
        const bool hintsOk=RestoreHints(parameters,*saved);
        const bool keysOk=!keys||RestoreCreateKeys(parameters,*keys);
        if(restored)*restored=hintsOk&&keysOk;
        if(!hintsOk||!keysOk)gReplayDenied.store(true,std::memory_order_release);
        request->Leave();
    }
}

NVSDK_NGX_Result CreateWithSelection(const CreateCall& create,
    const NVSDK_NGX_Parameter* parameters, NVSDK_NGX_Handle** output, const CreateRecord& record,
    bool* confirmed = nullptr,const CreateRecord* replay = nullptr) noexcept
{
    if(replay&&gReplayDenied.load(std::memory_order_acquire))return NVSDK_NGX_Result_FAIL_NotInitialized;
    const bool intercepted = IsOverrideFor(record.kind, record.preset) && RuntimeOverrideAllowed(record.kind);
    dlss_sr_override::Request request(record.kind, record.preset, intercepted,
        gRuntimeEligibility.load(std::memory_order_acquire));
    const SavedCreateKeys keys = replay?ApplyCreateKeys(parameters,*replay):SavedCreateKeys{};
    if(!keys.valid){if(!RestoreCreateKeys(parameters,keys))gReplayDenied.store(true);return NVSDK_NGX_Result_FAIL_InvalidParameter;}
    const SavedHints saved = replay&&!IsOverrideFor(record.kind,record.preset)
        ?ReplayHints(parameters,*replay):ApplyHints(record.kind, parameters, record.preset);
    if(!saved.valid)
    {
        const bool hintsOk=RestoreHints(parameters,saved),keysOk=RestoreCreateKeys(parameters,keys);
        if(!hintsOk||!keysOk)gReplayDenied.store(true);
        return NVSDK_NGX_Result_FAIL_InvalidParameter;
    }
    bool restored=true;
    const NVSDK_NGX_Result result = InvokeCreate(create, FeatureOf(record.kind), parameters, output, &saved, &request,&keys,&restored);
    if(!restored)Log(L"DLSS live creation parameter restoration failed; further recreation disabled until restart");
    if (confirmed) *confirmed = restored&&(!intercepted || request.Confirmed());
    return result;
}

void ReleaseOurs(ReleaseFn release, uintptr_t handle, const wchar_t* why) noexcept
{
    if (!release || !handle) return;
    const NVSDK_NGX_Result result = release(reinterpret_cast<NVSDK_NGX_Handle*>(handle));
    wchar_t text[160]{};
    swprintf_s(text, L"DLSS replaced handle released (%ls): handle=%p result=0x%08X", why,
        reinterpret_cast<void*>(handle), static_cast<uint32_t>(result));
    Log(text);
}
}

void SetRequested(uint32_t preset) noexcept { SetSelection(Kind::eSuperResolution, preset); }
uint32_t Requested() noexcept { return GetSelection(Kind::eSuperResolution); }
void SetRequestedRr(uint32_t preset) noexcept { SetSelection(Kind::eRayReconstruction, preset); }
uint32_t RequestedRr() noexcept { return GetSelection(Kind::eRayReconstruction); }
uint32_t RequestedFor(Kind kind) noexcept { return GetSelection(kind); }

void SetInitialSelectionLoader(InitialSelectionLoader loader) noexcept
{
    gSelections[Index(Kind::eSuperResolution)].loader.store(loader, std::memory_order_release);
}

void SetInitialRrSelectionLoader(InitialSelectionLoader loader) noexcept
{
    gSelections[Index(Kind::eRayReconstruction)].loader.store(loader, std::memory_order_release);
}

bool Active() noexcept
{
    return gTracked.load(std::memory_order_acquire) || gRetiredPending.load(std::memory_order_acquire);
}

void SetLogCallback(LogCallback callback) noexcept { gLog.store(callback, std::memory_order_release); }

void SetDrsHookInstaller(DrsHookInstaller installer) noexcept
{
    gInstaller.store(installer, std::memory_order_release);
}
void SetRuntimeEligibilityProbe(RuntimeEligibilityProbe probe) noexcept
{ gRuntimeEligibility.store(probe,std::memory_order_release); }

void PrepareForDevice(ID3D12Device* device) noexcept
{
    const auto installer = gInstaller.load(std::memory_order_acquire);
    if (!installer || !device || gDrsHookReady.load(std::memory_order_acquire)) return;
    if (installer(device)) gDrsHookReady.store(true, std::memory_order_release);
}

void PrepareForVulkan() noexcept
{
    const auto installer = gInstaller.load(std::memory_order_acquire);
    if (!installer || gDrsHookReady.load(std::memory_order_acquire)) return;
    if (installer(nullptr)) gDrsHookReady.store(true, std::memory_order_release);
}

NVSDK_NGX_Result Create(size_t route, const CreateCall& original, NVSDK_NGX_Feature feature,
    const NVSDK_NGX_Parameter* parameters, NVSDK_NGX_Handle** output) noexcept
{
    if (!Callable(original)) return NVSDK_NGX_Result_FAIL_NotInitialized;
    CreateRecord record{};
    record.route = route;
    record.kind = feature == NVSDK_NGX_Feature_SuperSampling ? Kind::eSuperResolution : Kind::eRayReconstruction;
    record.api = original.api;
    record.device = original.api == CreateApi::eVulkan1 ? reinterpret_cast<uintptr_t>(original.device) : 0;
    record.parameters = reinterpret_cast<uintptr_t>(parameters);
    record.preset = RequestedFor(record.kind);
    record.keys = ReadKeys(parameters);
    CaptureHints(record,parameters);
    record.gameHint = ReadGameHint(record.kind, parameters, record.keys.perfQuality);
    // Covers a later NGX initialization of this process.
    if (!gDrsHookReady.load(std::memory_order_acquire))
    {
        if (original.api != CreateApi::eD3D12) PrepareForVulkan();
        else if (ID3D12Device* device = CommandDevice(static_cast<ID3D12GraphicsCommandList*>(original.command)))
        {
            PrepareForDevice(device);
            device->Release();
        }
    }
    bool confirmed = false;
    NVSDK_NGX_Result result = CreateWithSelection(original, parameters, output, record, &confirmed);
    if (result == NVSDK_NGX_Result_Success && !confirmed)
    {
        // Preserve the game's successful native handle, but never report that
        // an unconfirmed preset displaced its cached driver override.
        record.preset = kGameOrDriver;
        Log(L"DLSS cached preset override not confirmed on Create; native handle preserved");
    }
    if (result != NVSDK_NGX_Result_Success && IsOverrideFor(record.kind, record.preset))
    {
        // DLSS refused the game's Create with our preset (for example RR preset F
        // below driver 580): the game gets its own settings instead.
        LogCreate(record, result, L"Create refused with the selection; retried with the game's hints");
        gCreateRetries.fetch_add(1, std::memory_order_relaxed);
        record.preset = kGameOrDriver;
        result = Invoke(original, feature, parameters, output);
    }
    record.handle = CreatedHandle(result, output);
    if (record.handle)
    {
        std::lock_guard lock(gMutex);
        gTracker.Created(record, NowMs());
        Publish();
    }
    const uint64_t count = gCreates.fetch_add(1, std::memory_order_relaxed) + 1;
    if (count <= 32 || IsOverrideFor(record.kind, record.preset)) LogCreate(record, result, L"Create");
    return result;
}

const NVSDK_NGX_Handle* Evaluate(size_t route, const CreateCall& create, ReleaseFn release,
    const NVSDK_NGX_Handle* handle, const NVSDK_NGX_Parameter* parameters) noexcept
{
    if (!handle || !Active()) return handle;
    const uintptr_t game = reinterpret_cast<uintptr_t>(handle);
    const uint64_t now = NowMs();
    Kind kind = Kind::eSuperResolution;
    bool tracked = false;
    std::array<uintptr_t, 8> due{};
    size_t dueCount = 0;
    {
        std::lock_guard lock(gMutex);
        tracked = gTracker.Tracked(route, game, &kind);
        if (tracked)
        {
            dueCount = gTracker.DueRetired(now, due);
            Publish();
        }
    }
    if (!tracked) return handle;
    for (size_t i = 0; i != dueCount; ++i) ReleaseOurs(release, due[i], L"retired");
    const uint32_t selection = RequestedFor(kind);
    const bool fixed = gFixedByDriver[Index(kind)].load(std::memory_order_acquire)
        && !RuntimeOverrideAllowed(kind);
    bool needKeys = false;
    {
        std::lock_guard lock(gMutex);
        needKeys = gTracker.NeedsKeys(route, game, selection, fixed);
    }
    const CreateKeys keys = needKeys ? ReadKeys(parameters) : CreateKeys{};
    Plan plan{};
    {
        std::lock_guard lock(gMutex);
        plan = gTracker.Evaluate(route, game, reinterpret_cast<uintptr_t>(parameters),
            needKeys ? &keys : nullptr, selection, now, fixed);
        Publish();
    }
    if (plan.action == Action::eForward)
        return plan.handle ? reinterpret_cast<const NVSDK_NGX_Handle*>(plan.handle) : handle;

    CreateRecord evidence = plan.creation;
    evidence.parameters = reinterpret_cast<uintptr_t>(parameters);
    evidence.preset = plan.preset;
    if(keys!=evidence.keys)Log(L"DLSS live switch replays the tracked handle's original creation values; current shared parameters restored before Evaluate");
    NVSDK_NGX_Handle* created = nullptr;
    NVSDK_NGX_Result result = NVSDK_NGX_Result_FAIL_NotInitialized;
    bool confirmed = false;
    const CreateCall recreate = RecreateCall(create, plan.creation);
    if (Callable(recreate) && release && recreate.command && parameters)
        result = CreateWithSelection(recreate, parameters, &created, evidence, &confirmed,&plan.creation);
    if (result == NVSDK_NGX_Result_Success && !confirmed)
    {
        ReleaseOurs(release, reinterpret_cast<uintptr_t>(created), L"cached override not confirmed");
        created = nullptr;
        result = NVSDK_NGX_Result_FAIL_NotInitialized;
    }
    const uintptr_t createdHandle = CreatedHandle(result, &created);
    evidence.handle = createdHandle;
    bool adopted = false;
    uintptr_t use = 0;
    {
        std::lock_guard lock(gMutex);
        use = gTracker.Recreated(route, game, plan.preset, createdHandle, NowMs(), adopted);
        Publish();
    }
    if (createdHandle && !adopted) ReleaseOurs(release, createdHandle, L"not adopted");
    LogCreate(evidence, result, adopted ? L"live preset switch" : L"live preset switch failed");
    return use ? reinterpret_cast<const NVSDK_NGX_Handle*>(use) : handle;
}

void BeforeRelease(size_t route, ReleaseFn release, NVSDK_NGX_Handle* handle) noexcept
{
    if (!handle || !gTracked.load(std::memory_order_acquire)) return;
    std::array<uintptr_t, 9> ours{};
    size_t count = 0;
    {
        std::lock_guard lock(gMutex);
        count = gTracker.Releasing(route, reinterpret_cast<uintptr_t>(handle), ours);
        Publish();
    }
    for (size_t i = 0; i != count; ++i) ReleaseOurs(release, ours[i], L"game release");
}

void AfterRelease(size_t route, NVSDK_NGX_Handle* handle, NVSDK_NGX_Result result) noexcept
{
    if (!handle || !gTracked.load(std::memory_order_acquire)) return;
    std::lock_guard lock(gMutex);
    gTracker.Released(route, reinterpret_cast<uintptr_t>(handle), result == NVSDK_NGX_Result_Success);
    Publish();
}

int32_t FilterDrsRead(uint32_t id, int32_t result, dlssg_preset::Setting* setting, bool writable,
    HMODULE caller) noexcept
{
    Kind kind{};
    if (!KindForDrsKey(id, kind)) return result;
    const uint64_t call = gDrsCalls.fetch_add(1, std::memory_order_relaxed) + 1;
    const bool provider = IsProvider(caller);
    if (!provider) gDrsNotProvider.fetch_add(1, std::memory_order_relaxed);
    else if (!setting || !writable) gDrsNotWritable.fetch_add(1, std::memory_order_relaxed);
    if (!provider || !setting || !writable)
    {
        if (call <= 8)
        {
            wchar_t text[192]{};
            swprintf_s(text, L"DLSS %ls DRS read left alone: call=%llu provider=%d writable=%d result=%d caller=%p",
                KindName(kind), static_cast<unsigned long long>(call), provider, writable, result,
                reinterpret_cast<void*>(caller));
            Log(text);
        }
        return result;
    }
    // Install at the provider's initialization read, before its first Create.
    // Discovery is kind-specific and pins the module; no heap address is kept.
    const auto eligible=gRuntimeEligibility.load(std::memory_order_acquire);
    const bool runtimeOverride = eligible && eligible() && gDrsHookReady.load(std::memory_order_acquire)
        && dlss_sr_override::Ensure(caller, kind);
    const uint32_t selection = RequestedFor(kind);
    uint32_t value = 0;
    const bool valued = DriverValue(id, result, *setting, value);
    int32_t answer = result;
    // A structurally supported runtime route preserves the NVIDIA app answer.
    // Its selector will ignore that cached value only during our own Creates,
    // so Game/driver works again immediately, including selected-at-start runs.
    const bool suppressed = !runtimeOverride && SuppressDrsRead(id, result, selection, answer);
    bool fixed = false;
    {
        std::lock_guard lock(gDrsMutex);
        DrsState& state = gDrs[Index(kind)];
        state.observed = true;
        ++state.reads;
        if (suppressed) ++state.suppressedReads;
        if (valued)
        {
            state.driverValueValid = true;
            state.driverValue = value;
        }
        state.suppressed = suppressed;
        fixed = FixedByDriver(state.observed, state.driverValueValid, state.driverValue, state.suppressed);
        gFixedByDriver[Index(kind)].store(fixed, std::memory_order_release);
    }
    wchar_t text[256]{};
    swprintf_s(text, L"DLSS %ls provider DRS preset read: result=%d driver=%ls selection=%ls answer=%ls fixedByDriver=%d runtimeOverride=%d",
        KindName(kind), result, valued ? PresetName(value) : L"none",
        selection ? PresetName(selection) : L"game/driver",
        suppressed ? L"not set (creation hints decide)" : L"driver's", fixed, runtimeOverride);
    Log(text);
    return answer;
}

Snapshot Read() noexcept
{
    Snapshot snapshot{};
    snapshot.requested = Requested();
    snapshot.requestedRr = RequestedRr();
    const uint64_t now = NowMs();
    {
        std::lock_guard lock(gMutex);
        snapshot.counters = gTracker.Read();
        snapshot.primary = gTracker.Primary(Kind::eSuperResolution);
        snapshot.rrPrimary = gTracker.Primary(Kind::eRayReconstruction);
        snapshot.running = gTracker.Running(Kind::eSuperResolution, now);
        snapshot.rayReconstructionRunning = gTracker.Running(Kind::eRayReconstruction, now);
    }
    {
        std::lock_guard lock(gDrsMutex);
        DrsView* views[]{&snapshot.sr, &snapshot.rr};
        for (size_t i = 0; i != 2; ++i)
        {
            const DrsState& state = gDrs[i];
            DrsView& view = *views[i];
            view.observed = state.observed;
            view.driverValueValid = state.driverValueValid;
            view.driverValue = state.driverValue;
            view.suppressed = state.suppressed;
            view.overrideCached = FixedByDriver(state.observed, state.driverValueValid, state.driverValue, state.suppressed);
            // A verified runtime selector ignores the cached driver value during our Creates.
            view.fixedByDriver = view.overrideCached
                && !RuntimeOverrideAllowed(i == 0 ? Kind::eSuperResolution : Kind::eRayReconstruction);
            view.reads = state.reads;
            view.suppressedReads = state.suppressedReads;
        }
    }
    snapshot.detected = snapshot.counters.srCreates != 0;
    snapshot.rrDetected = snapshot.counters.rrCreates != 0;
    snapshot.drsNotProvider = gDrsNotProvider.load(std::memory_order_relaxed);
    snapshot.drsNotWritable = gDrsNotWritable.load(std::memory_order_relaxed);
    snapshot.createRetries = gCreateRetries.load(std::memory_order_relaxed);
    return snapshot;
}

#if defined(MFG_DLSS_SR_TESTING)
namespace testing
{
void SetClock(Clock clock) noexcept { gClock.store(clock, std::memory_order_release); }
void SetProviderCheck(ProviderCheck check) noexcept { gProviderCheck.store(check, std::memory_order_release); }
void Reset() noexcept
{
    gReplayDenied.store(false,std::memory_order_release);
    gDrsHookReady.store(false,std::memory_order_release);
    {
        std::lock_guard lock(gMutex);
        gTracker = {};
        Publish();
    }
    {
        std::lock_guard lock(gDrsMutex);
        for (auto& state : gDrs) state = {};
        for (auto& fixed : gFixedByDriver) fixed.store(false);
    }
    for (auto& selection : gSelections)
    {
        selection.value.store(kGameOrDriver);
        selection.initialized.store(false);
        selection.loader.store(nullptr);
    }
    gDrsCalls.store(0);
    gDrsNotProvider.store(0);
    gDrsNotWritable.store(0);
    gCreates.store(0);
    gCreateRetries.store(0);
}
}
#endif
}
