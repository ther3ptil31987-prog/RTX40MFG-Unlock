#include "sdk_pacing.h"
#include "sdk_pacing_dlssg.h"
#include "sdk_bulk_profile.h"
#include "xess_policy.h"
#include "xess_mod.h"
#include "overlay_device_identity.h"
#include "port/port_support.h"
#include "third_party/minhook/include/MinHook.h"
#include <intrin.h>
#include <algorithm>
#include <atomic>
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
// The profiled presenter prologue establishes rbp = caller-rsp + 0x89.
// At the intercepted call the return slot is caller-rsp - 8, hence:
constexpr uintptr_t kSummaryFromReturn = 0x48; // [rbp - 0x49]
constexpr uintptr_t kBundleFromReturn = 0xf8;  // [rbp + 0x67]
using Present = HRESULT(*)(void*, UINT, UINT, uint64_t, const DXGI_PRESENT_PARAMETERS*, ID3D12Resource* const*, bool);
using Pace = bool(*)(void*, const void*, uint8_t, const void*, uint32_t);
using Deadline = int64_t*(*)(void*, int64_t*, const void*, const void*, uint32_t, uint32_t);

struct Owner
{
    void* handle{};
    uint64_t generation{};
    uintptr_t implementation{}, chain{}, presenter{}, native{}, device{};
    std::atomic<unsigned> pacedLogs{}, skippedLogs{};
    std::atomic<bool> disabled{}, retired{};
    // dev.42: the presentation queue of an owner observed for the diagnostic capture
    // (ObservePlain). Never used for pacing.
    Microsoft::WRL::ComPtr<ID3D12CommandQueue> captureQueue;
    std::mutex clockMutex;
    // DLSS-G CPU pacer interval for the SDK's own pacing (all counts).
    cadence::BatchIntervalEstimator batchInterval;
    std::atomic<unsigned> intervalLogs{};
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

// Every field read from the SDK must be a readable, pinned COM interface before
// invoking it. Canonical identity is established with public unwrapping/QI.
bool PinnedInterface(IUnknown* object, size_t last) noexcept
{
    uintptr_t table{};
    std::array<uintptr_t, 44> entries{};
    return last < entries.size() && Value(uintptr_t(object), table)
        && Read(table, entries.data(), (last+1)*sizeof(uintptr_t))
        && single_overlay::native::PinInterface(object, last);
}

bool DeviceMatches(ID3D12DeviceChild* child, IUnknown* expected) noexcept
{
    Microsoft::WRL::ComPtr<ID3D12Device> device;
    Microsoft::WRL::ComPtr<IUnknown> identity;
    return expected && PinnedInterface(child, 7)
        && SUCCEEDED(child->GetDevice(IID_PPV_ARGS(&device))) && device
        && single_overlay::dx12::DeviceIdentity(device.Get(), identity)
        && identity.Get() == expected;
}

bool NativeQueue(ID3D12CommandQueue* input, IUnknown* deviceIdentity,
    Microsoft::WRL::ComPtr<ID3D12CommandQueue>& native,
    Microsoft::WRL::ComPtr<IUnknown>& identity) noexcept
{
    native.Reset(); identity.Reset();
    return PinnedInterface(input, 2) && single_overlay::native::Unwrap(input, native)
        && PinnedInterface(native.Get(), 18) && DeviceMatches(native.Get(), deviceIdentity)
        && native->GetDesc().Type == D3D12_COMMAND_LIST_TYPE_DIRECT
        && SUCCEEDED(native.As(&identity)) && identity;
}

bool ContextParts(const Route& current, void* handle, uintptr_t& implementation, uintptr_t& chain) noexcept
{
    uint32_t signature{}; uintptr_t table{}, getter{};
    const auto base = uintptr_t(current.module);
    return Value(uintptr_t(handle), signature) && signature == 0x47466558
        && Value(uintptr_t(handle)+8, implementation)
        && Value(implementation, table) && table == base+kImplementationTable
        && Value(table+0x80, getter) && getter == base+kGetter
        && Value(implementation+8, chain)
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
        || !Value(owner.chain+0x120, currentPresenter) || currentPresenter != owner.presenter
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
}

bool Live(const Owner& owner) noexcept
{
    if (owner.retired.load(std::memory_order_acquire) || owner.disabled.load(std::memory_order_acquire)) return false;
    uint32_t magic{}; uintptr_t implementation{}, chain{}, presenter{}, native{}, device{};
    uint8_t intel{}, hardware{}, software{};
    return Value(uintptr_t(owner.handle), magic) && magic == 0x47466558
        && Value(uintptr_t(owner.handle)+8, implementation) && implementation == owner.implementation
        && Value(implementation+8, chain) && chain == owner.chain
        && Value(chain+0x120, presenter) && presenter == owner.presenter
        && Value(chain+0x30, native) && native == owner.native
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

// File name of the module that holds address (empty if none): tells whether the
// SDK's native swapchain is DXGI's own object or a layer (Streamline, OptiScaler).
std::string ModuleOf(const void* address)
{
    HMODULE module{};
    wchar_t path[MAX_PATH]{};
    if (!address || !GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCWSTR>(address), &module) || !GetModuleFileNameW(module, path, MAX_PATH)) return {};
    const wchar_t* name = wcsrchr(path, L'\\'); name = name ? name+1 : path;
    char narrow[MAX_PATH]{};
    WideCharToMultiByte(CP_UTF8, 0, name, -1, narrow, int(sizeof(narrow)), nullptr, nullptr);
    std::string text(narrow);
    for (auto& c : text) if (c == '"' || c == '\\' || unsigned(c) < 0x20) c = '_';
    return text;
}

// dev.20/dev.37: the SDK presents straight to DXGI's own swapchain (dev.19 log:
// vtable_module and present1_module dxgi.dll), so no proxy sees its Present1. For an
// observed owner that object's vtable pointer is swapped to a copy of DXGI's table
// whose Present1 slot runs the capture's copy first, then calls DXGI's Present1. Every
// other slot (ResizeBuffers included: the game's buffer count is kept) and the RTTI
// locator before the table are DXGI's; other swapchains keep DXGI's table. All DXGI
// swapchains share one vtable, so one copy serves every SDK swapchain. It lives in this
// module's image (.data), so COM pinning checks (PinInterface/ImageSlot) still accept the
// object. The object is never restored: it may already be released when its owner retires.
using Present1Fn = HRESULT (STDMETHODCALLTYPE*)(IDXGISwapChain1*, UINT, UINT, const DXGI_PRESENT_PARAMETERS*);
constexpr size_t kPresent1Slot = 22;
constexpr size_t kMinimumSwapchainSlots = 41; // IUnknown .. IDXGISwapChain4
constexpr size_t kMaximumVtableSlots = 128;
std::atomic<Present1Fn> dxgiPresent1{};
alignas(64) void* present1Table[kMaximumVtableSlots+1]{};
void** present1Source{}; // DXGI's table that present1Table copies (present1Mutex)
size_t present1Slots{};
std::mutex present1Mutex;

// dev.37 diagnostic final-present capture (SetPresentCapture).
std::atomic<const PresentCaptureHooks*> presentCapture{};

HRESULT STDMETHODCALLTYPE CaptureDxgiPresent1(IDXGISwapChain1* self, UINT sync, UINT flags,
    const DXGI_PRESENT_PARAMETERS* parameters)
{
    const auto original = dxgiPresent1.load(std::memory_order_acquire);
    if (!(flags & DXGI_PRESENT_TEST))
        if (const auto capture = presentCapture.load(std::memory_order_acquire)) capture->beforePresent1(self);
    return original(self, sync, flags, parameters);
}

bool ImageBounds(HMODULE module, uintptr_t& begin, uintptr_t& end) noexcept
{
    const auto base = reinterpret_cast<const unsigned char*>(module);
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (!base || dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base+dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return false;
    begin = uintptr_t(base); end = begin+nt->OptionalHeader.SizeOfImage;
    return true;
}

// Returns nullptr when installed, else the reason it was not.
const char* InstallPresent1Gate(const Owner& owner, size_t& slots) noexcept
{
    slots = 0;
    auto* native = reinterpret_cast<IDXGISwapChain1*>(owner.native);
    if (!native || !PinnedInterface(native, 40)) return "native_interface";
    void** const vtable = *reinterpret_cast<void***>(native);
    if (vtable == present1Table+1) return "already";
    uintptr_t begin{}, end{};
    if (!ImageBounds(GetModuleHandleW(L"dxgi.dll"), begin, end)) return "dxgi_module";
    const auto inImage = [&](const void* address, size_t bytes = 1) noexcept
    { return uintptr_t(address) >= begin && uintptr_t(address) <= end-bytes; };
    if (!inImage(vtable-1, (kMinimumSwapchainSlots+1)*sizeof(void*))) return "vtable_not_dxgi";
    std::lock_guard lock(present1Mutex);
    if (present1Source)
    {
        if (present1Source != vtable) return "vtable_differs";
    }
    else
    {
        size_t count{};
        while (count < kMaximumVtableSlots && inImage(vtable+count, sizeof(void*)) && inImage(vtable[count])) ++count;
        if (count < kMinimumSwapchainSlots) return "vtable_short";
        dxgiPresent1.store(reinterpret_cast<Present1Fn>(vtable[kPresent1Slot]), std::memory_order_release);
        present1Table[0] = vtable[-1]; // MSVC RTTI complete object locator
        std::copy(vtable, vtable+count, present1Table+1);
        present1Table[1+kPresent1Slot] = reinterpret_cast<void*>(&CaptureDxgiPresent1);
        present1Slots = count; present1Source = vtable;
    }
    // Readers load the pointer once per call; either table is valid at any time.
    if (InterlockedCompareExchangePointer(reinterpret_cast<void* volatile*>(native), present1Table+1, vtable) != vtable)
        return "vtable_raced";
    slots = present1Slots;
    return nullptr;
}

// Returns nullptr when installed, else the reason it was not.
const char* ArmPresent1Gate(const Owner& owner) noexcept
{
    const char* reason = "exception";
    try
    {
        char value[8]{};
        const DWORD length = GetEnvironmentVariableA("RTXMFG_XESS_PRESENT1_GATE", value, DWORD(sizeof(value)));
        size_t slots{};
        reason = length == 1 && value[0] == '0' ? "disabled" : InstallPresent1Gate(owner, slots);
        xess_mod::WriteLog("flip_meter_present1_vtable", "\"generation\":"+std::to_string(owner.generation)+
            ",\"installed\":"+(reason ? "false" : "true")+",\"reason\":\""+(reason ? reason : "none")+
            "\",\"slots\":"+std::to_string(slots));
    }
    catch (...) {}
    return reason;
}

// dev.49: the capture's Present1 slot on a layered native swap chain. Under OptiScaler
// (Witcher, dev.48 log) the swap chain XeFG presents to is another layer's wrapper, not
// DXGI's object (flip_meter_present1_vtable "vtable_not_dxgi"), so the capture never saw
// a Present1 and stayed requested. For an owner observed for the capture, that object's
// vtable pointer is swapped to a copy of the layer's own table whose Present1 slot runs
// the capture's copy, then the layer's Present1. Nothing else changes. The layer's table
// must be read-only image memory (PinInterface pins its module) whose executable run
// (TableSlots) holds IDXGISwapChain4 and the class's own virtuals, such as the deleting
// destructor Release reaches. One layer table at a time; never restored, like the DXGI slot.
alignas(64) void* captureTable[kMaximumVtableSlots+1]{};
void** captureSource{}; // the layer's table that captureTable copies (present1Mutex)
size_t captureSlots{};
std::atomic<Present1Fn> layerPresent1{};

HRESULT STDMETHODCALLTYPE CapturePresent1(IDXGISwapChain1* self, UINT sync, UINT flags,
    const DXGI_PRESENT_PARAMETERS* parameters)
{
    const auto original = layerPresent1.load(std::memory_order_acquire);
    if (!(flags & DXGI_PRESENT_TEST))
        if (const auto capture = presentCapture.load(std::memory_order_acquire)) capture->beforePresent1(self);
    return original(self, sync, flags, parameters);
}

bool ReadOnlyImage(const void* address) noexcept
{
    MEMORY_BASIC_INFORMATION info{};
    return VirtualQuery(address, &info, sizeof(info)) == sizeof(info) && info.State == MEM_COMMIT
        && info.Type == MEM_IMAGE && (info.Protect & 0xff) == PAGE_READONLY && !(info.Protect & PAGE_GUARD);
}

bool ExecutableMemory(uintptr_t address) noexcept
{
    MEMORY_BASIC_INFORMATION info{};
    if (address < 0x10000 || VirtualQuery(reinterpret_cast<const void*>(address), &info, sizeof(info)) != sizeof(info)) return false;
    constexpr DWORD kExecute = PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
    return info.State == MEM_COMMIT && (info.Protect & kExecute) && !(info.Protect & PAGE_GUARD);
}

// The slots a table holds: consecutive entries that point into executable memory
// (interface methods, then the class's own virtuals such as the deleting
// destructor that `delete this` in Release reaches through this very table). The
// next table's RTTI locator or any data ends the run.
size_t TableSlots(void** source) noexcept
{
    size_t count{};
    uintptr_t entry{};
    while (count < kMaximumVtableSlots && Value(uintptr_t(source+count), entry) && ExecutableMemory(entry)) ++count;
    return count;
}

bool OurTable(void** table) noexcept
{
    return table == present1Table+1 || table == captureTable+1;
}

// Returns nullptr when installed, else the reason it was not; source = the object's table.
const char* InstallCapturePresent1(const Owner& owner, size_t& slots, void**& source) noexcept
{
    slots = 0; source = nullptr;
    auto* native = reinterpret_cast<IDXGISwapChain1*>(owner.native);
    if (!native || !PinnedInterface(native, 40)) return "native_interface";
    void** const vtable = *reinterpret_cast<void***>(native);
    source = vtable;
    if (vtable == captureTable+1) return "already";
    if (OurTable(vtable)) return "vtable_ours";
    const size_t count = TableSlots(vtable);
    if (count < kMinimumSwapchainSlots) return "vtable_short";
    if (count >= kMaximumVtableSlots) return "vtable_long";
    if (!ReadOnlyImage(vtable) || !ReadOnlyImage(vtable+count-1)) return "vtable_writable";
    uintptr_t locator{};
    if (!Value(uintptr_t(vtable-1), locator)) return "vtable_locator";
    std::lock_guard lock(present1Mutex);
    if (captureSource)
    {
        if (captureSource != vtable) return "vtable_differs";
    }
    else
    {
        layerPresent1.store(reinterpret_cast<Present1Fn>(vtable[kPresent1Slot]), std::memory_order_release);
        captureTable[0] = reinterpret_cast<void*>(locator); // MSVC RTTI complete object locator
        std::copy(vtable, vtable+count, captureTable+1);
        captureTable[1+kPresent1Slot] = reinterpret_cast<void*>(&CapturePresent1);
        captureSlots = count; captureSource = vtable;
    }
    if (InterlockedCompareExchangePointer(reinterpret_cast<void* volatile*>(native), captureTable+1, vtable) != vtable)
        return "vtable_raced";
    slots = captureSlots;
    return nullptr;
}

void ArmCapturePresent1(const Owner& owner) noexcept
{
    try
    {
        size_t slots{};
        void** source{};
        const char* reason = InstallCapturePresent1(owner, slots, source);
        xess_mod::WriteLog("capture_present1_vtable", "\"generation\":"+std::to_string(owner.generation)+
            ",\"installed\":"+(reason ? "false" : "true")+",\"reason\":\""+(reason ? reason : "none")+
            "\",\"slots\":"+std::to_string(slots)+",\"module\":\""+ModuleOf(source)+"\"");
    }
    catch (...) {}
}

// dev.42: an owner observed for the diagnostic final-present capture: the SDK's presentation
// queue, read from the presenter's copy queue field and checked against the chain's, and the
// Present1 slot. Pacing is unchanged; any failure only leaves the capture without this owner.
// dev.49: a layered native swap chain gets the capture-only Present1 slot instead
// (ArmCapturePresent1), only while the capture's hooks are registered.
void ObservePlain(Owner& owner, ID3D12Device* device) noexcept
{
    const bool capture = presentCapture.load(std::memory_order_acquire) != nullptr;
    const char* reason{};
    try
    {
        Microsoft::WRL::ComPtr<IUnknown> deviceIdentity, queueIdentity;
        uintptr_t presenterQueue{}, chainQueue{};
        if (!single_overlay::dx12::DeviceIdentity(device, deviceIdentity)) reason = "device_identity";
        else if (!Value(owner.presenter+bulk_profile::kPresenterCopyQueueOffset, presenterQueue) || !presenterQueue
            || !Value(owner.chain+0x28, chainQueue) || chainQueue != presenterQueue) reason = "queue_layout";
        else if (!NativeQueue(reinterpret_cast<ID3D12CommandQueue*>(presenterQueue), deviceIdentity.Get(),
            owner.captureQueue, queueIdentity)) reason = "queue_identity";
    }
    catch (...) { reason = "exception"; }
    if (reason) owner.captureQueue.Reset();
    else if (const char* gate = ArmPresent1Gate(owner); gate && !strcmp(gate, "vtable_not_dxgi") && capture)
        ArmCapturePresent1(owner);
    try
    {
        xess_mod::WriteLog("capture_observer", "\"generation\":"+std::to_string(owner.generation)+
            ",\"observed\":"+(reason ? "false" : "true")+",\"capture\":"+(capture ? "true" : "false")+
            (reason ? ",\"reason\":\""+std::string(reason)+"\"" : ""));
    }
    catch (...) {}
}

// dev.37: the observed owner whose native Present this is, and which output of a generating
// batch it presents (eOther: any other Present on that presenter).
std::shared_ptr<Owner> ObservedOwner(const Route& current, const Invocation& call, OutputKind& kind)
{
    const auto offset = call.returnAddress-uintptr_t(current.module);
    kind = offset == kFirstReturn ? OutputKind::eFirst : offset == kExtraReturn ? OutputKind::eExtra
        : offset == kRealReturn ? OutputKind::eReal : OutputKind::eOther;
    if (const auto owners = current.owners.load(std::memory_order_acquire))
        for (const auto& owner : *owners)
            if (owner->presenter == uintptr_t(call.presenter) && owner->captureQueue
                && !owner->retired.load(std::memory_order_acquire) && !owner->disabled.load(std::memory_order_acquire))
                return owner;
    return {};
}

HRESULT Dispatch(Route& current, const Invocation& call)
{
    const DWORD incomingError = GetLastError();
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
    // dev.37 diagnostic capture: which output this Present is, on the SDK's presentation queue.
    if (const auto capture = presentCapture.load(std::memory_order_acquire))
    {
        std::shared_ptr<Owner> observedOwner;
        OutputKind outputKind{OutputKind::eOther};
        try { observedOwner = ObservedOwner(current, call, outputKind); } catch (...) {}
        if (observedOwner) capture->beforeOutput(outputKind, uint32_t(call.frame >> 32), observedOwner->captureQueue.Get());
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

// The SDK's own Pace: replace only the arithmetic deadline with the DLSS-G interval.
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
        if (owner.disabled.load(std::memory_order_acquire) || owner.retired.load(std::memory_order_acquire)
            || !StackRange(uintptr_t(output), sizeof(*output)) || !StackRange(uintptr_t(record), 0x28)
            || !Value(uintptr_t(record), timestamp) || timestamp <= 0
            || !Value(uintptr_t(record)+0x10, frame) || !Value(uintptr_t(record)+0x20, batchStart)
            || !Live(owner)) return;
        const auto now = NowNs();
        cadence::BatchIntervalEstimator::Decision decision{};
        bool planned{};
        {
            std::lock_guard lock(owner.clockMutex);
            if (index == 1) owner.batchInterval.Observe(frame, total, batchStart);
            planned = owner.batchInterval.Plan(frame, total, index, now, decision);
        }
        const auto sdkDeadline = *output;
        if (planned) *output = decision.deadline;
        if (owner.intervalLogs.fetch_add(1, std::memory_order_relaxed) < 24 || (index == 1 && frame%120 == 0))
            xess_mod::WriteLog("dlssg_interval_deadline", "\"generation\":"+std::to_string(owner.generation)+
                ",\"frame\":"+std::to_string(frame)+",\"index\":"+std::to_string(index)+",\"outputs\":"+std::to_string(total)+
                ",\"applied\":"+(planned ? "true" : "false")+",\"interval_ns\":"+std::to_string(decision.interval)+
                ",\"samples\":"+std::to_string(decision.samples)+",\"capped\":"+(decision.capped ? "true" : "false")+
                ",\"batch_start_ns\":"+std::to_string(batchStart)+",\"now_ns\":"+std::to_string(now)+
                ",\"sdk_deadline_ns\":"+std::to_string(sdkDeadline)+",\"deadline_ns\":"+std::to_string(*output));
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
        kNativePresent=layout.Rva(0x21f730); kPace=layout.Rva(0x21ee30);
        kExtraReturn=layout.Rva(0x2202ed); kFirstReturn=layout.Rva(0x220db2); kRealReturn=layout.Rva(0x220467);
        kDeadlineReturn=layout.Rva(0x21ef96); kDeadline=layout.Rva(0x224b30);
        kImplementationTable=layout.Rva(0x157c3a0); kChainTable=layout.Rva(0x157a980); kGetter=layout.Rva(0xf410);
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
        current->enabled.store(true, std::memory_order_release);
        xess_mod::WriteLog("extra_pacing_hook", "\"native_present_rva\":"+std::to_string(kNativePresent)+
            ",\"extra_return_rva\":"+std::to_string(kExtraReturn)+",\"first_return_rva\":"+std::to_string(kFirstReturn)+
            ",\"real_return_rva\":"+std::to_string(kRealReturn)+",\"deadline_hook\":true,\"profile_verified\":true");
        return true;
    }
    catch (const std::exception& error) { failure = error.what(); }
    catch (...) { failure = "pacing_install_exception"; }
    return false;
}

bool Register(void* handle, uint64_t generation, ID3D12Device* device, std::string& failure, bool observe) noexcept
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
            || !Value(owner->chain+0x120, owner->presenter)
            || !Value(owner->chain+0x30, owner->native)
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
        if (observe) ObservePlain(*owner, device);
        next->push_back(owner);
        current->owners.store(next, std::memory_order_release);
        xess_mod::WriteLog("extra_pacing_owner", "\"generation\":"+std::to_string(generation)+
            ",\"handle\":"+std::to_string(uintptr_t(handle))+",\"sdk_swapchain\":"+std::to_string(owner->chain)+
            ",\"presenter\":"+std::to_string(owner->presenter)+",\"native_swapchain\":"+std::to_string(owner->native)+
            ",\"application_device\":"+std::to_string(uintptr_t(device))+
            ",\"presenter_device\":"+std::to_string(presenterDevice)+
            ",\"native_device_identity_used\":"+(presenterDevice != uintptr_t(device) ? "true" : "false")+
            ",\"observed\":"+(owner->captureQueue ? "true" : "false"));
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

void SetPresentCapture(const PresentCaptureHooks* hooks) noexcept
{
    presentCapture.store(hooks, std::memory_order_release);
}
}
