#include "xess_mod.h"
#include "xess_runtime.h"
#include "runtime_layout.h"
#include "xess_api.h"
#include "game_profile.h"
#include "../optiscaler_host.h"
#include <string>
#include <MinHook.h>
#include <Psapi.h>
#include <intrin.h>
#include <array>
#include <atomic>
#include <cstring>
#include <cwchar>
#include <mutex>
#include <vector>
#include <algorithm>
#include <type_traits>
#include "third_party/minhook/src/hde/hde64.h"

namespace xess_mod {
Api original;
namespace {
HMODULE selfModule{}, fgOwner{}, llOwner{};
// beta.8 923: OptiScaler took libxell's create and destroy entries (immutable after active's publication).
bool hostLatency{};
std::atomic<bool> active{}, prepared{};
std::atomic<bool> selfReady{};
std::mutex discoveryMutex;
bool attempted{};
std::atomic<decltype(&GetProcAddress)> rawGetProc{&GetProcAddress};
FARPROC RawProc(HMODULE module, LPCSTR name) noexcept { return rawGetProc.load(std::memory_order_acquire)(module, name); }
struct CreateSlot { std::atomic<HMODULE> owner{}; std::atomic<FARPROC> target{}; };
std::array<CreateSlot, 8> fgCreateSlots, llCreateSlots;
SRWLOCK slotLock = SRWLOCK_INIT;
bool resolverAttempted{};
bool HostCaller(void* address) noexcept;

template<bool Fg, size_t Index> struct FirstCreate {
    using Result = std::conditional_t<Fg, xefg_swapchain_result_t, xell_result_t>;
    using Handle = std::conditional_t<Fg, xefg_swapchain_handle_t, xell_context_handle_t>;
    using Function = Result(*)(ID3D12Device*, Handle*);
    static __declspec(noinline) Result Call(ID3D12Device* device, Handle* output)
    {
        auto& slot = (Fg ? fgCreateSlots : llCreateSlots)[Index];
        auto native = reinterpret_cast<Function>(slot.target.load(std::memory_order_acquire));
        if (!native) return static_cast<Result>(-1);
        // Discovery/hook activation runs at the API call, never inside resolver
        // callbacks or DLL load notifications. Private programs are published by
        // the backend before it invokes the private provider's first Create.
        DiscoverSdkModules();
        if (active.load(std::memory_order_acquire) && slot.owner.load(std::memory_order_acquire) == (Fg ? fgOwner : llOwner)) {
            HostCallScope host(HostCaller(_ReturnAddress()));
            if constexpr (Fg) return Hook_xefgSwapChainD3D12CreateContext(device, output);
            else return Hook_xellD3D12CreateContext(device, output);
        }
        return native(device, output);
    }
};
template<bool Fg, size_t... Index> auto CreateGateways(std::index_sequence<Index...>)
{ return std::array<FARPROC, sizeof...(Index)>{reinterpret_cast<FARPROC>(&FirstCreate<Fg, Index>::Call)...}; }
const auto fgCreateGateways = CreateGateways<true>(std::make_index_sequence<8>{});
const auto llCreateGateways = CreateGateways<false>(std::make_index_sequence<8>{});
bool OwnCode(HMODULE module, const void* code) noexcept;
bool PrivateModule(HMODULE module) noexcept;
bool SdkCaller(void* address) noexcept;

FARPROC WINAPI ResolveCreate(HMODULE module, LPCSTR name)
{
    const auto result = RawProc(module, name);
    const DWORD error = GetLastError();
    FARPROC replacement = result;
    if (result && uintptr_t(name) > 0xffff) {
        const bool fg = !strcmp(name, "xefgSwapChainD3D12CreateContext");
        const bool ll = !strcmp(name, "xellD3D12CreateContext");
        // The game's verified SDK pair may resolve another native provider.
        // Preserve these cached internal exports as well as same-module calls;
        // returning our gateway would change the forwarding adapter's owner.
        if ((fg || ll) && !(active.load(std::memory_order_acquire) && SdkCaller(_ReturnAddress()))
            && !OwnCode(module, _ReturnAddress()) && !PrivateModule(module)
            && OwnCode(module, reinterpret_cast<void*>(result))) {
            auto& slots = fg ? fgCreateSlots : llCreateSlots;
            const auto& gateways = fg ? fgCreateGateways : llCreateGateways;
            AcquireSRWLockExclusive(&slotLock);
            for (size_t i=0; i<slots.size(); ++i) {
                auto owner = slots[i].owner.load(std::memory_order_acquire);
                if (!owner) {
                    HMODULE pinned{};
                    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                        reinterpret_cast<LPCWSTR>(module), &pinned) || pinned != module) break;
                    slots[i].owner.store(module, std::memory_order_release);
                    slots[i].target.store(result, std::memory_order_release);
                    owner = module;
                }
                if (owner == module) { replacement = gateways[i]; break; }
            }
            ReleaseSRWLockExclusive(&slotLock);
        }
    }
    // UI gateways stay with RTXMFG's executable-scoped import hooks; this
    // resolver only routes the two XeSS SDK creation entry points.
    SetLastError(error);
    return replacement;
}
void InstallResolver() noexcept
{
    if (resolverAttempted || !selfReady.load(std::memory_order_acquire)) return;
    resolverAttempted = true;
    const auto init = MH_Initialize();
    if (init != MH_OK && init != MH_ERROR_ALREADY_INITIALIZED) return;
    HMODULE kernel = GetModuleHandleW(L"kernel32.dll");
    auto target = reinterpret_cast<void*>(RawProc(kernel, "GetProcAddress"));
    // The resolved system entry point may belong to kernelbase on modern Windows.
    HMODULE owner{};
    if (!target || !GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
        reinterpret_cast<LPCWSTR>(target), &owner) || !OwnCode(owner, target)) return;
    void* trampoline{};
    if (MH_CreateHook(target, reinterpret_cast<void*>(&ResolveCreate), &trampoline) != MH_OK) return;
    rawGetProc.store(reinterpret_cast<decltype(&GetProcAddress)>(trampoline), std::memory_order_release);
    const bool enabled = MH_EnableHook(target) == MH_OK;
    WriteLog("sdk_dynamic_resolver", std::string("\"enabled\":")+(enabled ? "true" : "false"));
}

bool SdkCaller(void* address) noexcept
{
    HMODULE owner{};
    return GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCWSTR>(address), &owner) && (owner == fgOwner || owner == llOwner);
}
// dev.40: calls from OptiScaler's own code (its frame generation output). The contexts they
// create stay untracked (xess_mod HostCallScope) and later calls on them go straight to the
// provider.
bool HostCaller(void* address) noexcept
{
    return optiscaler_host::Detected() && optiscaler_host::IsHostAddress(address);
}
// Every hooked XeFG entry point except creation takes its context first, as does every XeLL one.
template<class First, class... Rest> bool HostOwnedFirst(First first, Rest...) noexcept
{
    if constexpr (std::is_same_v<First, xefg_swapchain_handle_t>) return HostOwnedHandle(first, true);
    else if constexpr (std::is_same_v<First, xell_context_handle_t>) return HostOwnedHandle(first, false);
    else return false;
}
inline bool HostOwnedFirst() noexcept { return false; }
// dev.41: a host context's initialization and destruction still reach the backend, which
// registers and removes its plain pacing owner around the provider call.
template<auto H> constexpr bool kHostLifecycle = false;
template<> constexpr bool kHostLifecycle<&Hook_xefgSwapChainDestroy> = true;
template<> constexpr bool kHostLifecycle<&Hook_xefgSwapChainD3D12InitFromSwapChain> = true;
template<> constexpr bool kHostLifecycle<&Hook_xefgSwapChainD3D12InitFromSwapChainDesc> = true;
// Names the gateway's entry point once per process; defined after `entries`.
void LogFirstCall(void* gateway, void* caller) noexcept;
// All originals exist before hook publication. A partial cohort remains native.
// Native SDK self-calls must not turn its internal contexts into private ones.
template<auto Member, auto Hook, class Signature = decltype(Hook)> struct Gateway;
template<auto Member, auto Hook, class Result, class... Args>
struct Gateway<Member, Hook, Result(*)(Args...)> {
    static __declspec(noinline) Result Call(Args... args)
    {
        // Which SDK entry points a game actually uses (first call each).
        static std::atomic<bool> seen{};
        if (!seen.exchange(true, std::memory_order_relaxed)) LogFirstCall(reinterpret_cast<void*>(&Call), _ReturnAddress());
        if (!prepared.load(std::memory_order_acquire)) return static_cast<Result>(-1);
        if (!active.load(std::memory_order_acquire)) return (original.*Member)(args...);
        if (SdkCaller(_ReturnAddress())) return (original.*Member)(args...);
        const bool host = HostCaller(_ReturnAddress());
        if constexpr (!kHostLifecycle<Hook>)
            if (host && HostOwnedFirst(args...)) return (original.*Member)(args...);
        HostCallScope scope(host);
        return Hook(args...);
    }
};
struct Entry { const char* name; bool fg; void* hook; void** original; void* target{}; };
std::array entries{
#define FG_ENTRY(name) Entry{#name, true, reinterpret_cast<void*>(&Gateway<&Api::name, &Hook_##name>::Call), reinterpret_cast<void**>(&original.name)},
    XESS_FG_IMPORTS(FG_ENTRY)
#undef FG_ENTRY
#define LL_ENTRY(name) Entry{#name, false, reinterpret_cast<void*>(&Gateway<&Api::name, &Hook_##name>::Call), reinterpret_cast<void**>(&original.name)},
    XESS_LL_IMPORTS(LL_ENTRY)
#undef LL_ENTRY
};
void LogFirstCall(void* gateway, void* caller) noexcept
{
    try {
        const char* name = "unknown";
        for (const auto& entry : entries) if (entry.hook == gateway) { name = entry.name; break; }
        HMODULE owner{};
        wchar_t path[MAX_PATH]{};
        if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                reinterpret_cast<LPCWSTR>(caller), &owner))
            GetModuleFileNameW(owner, path, MAX_PATH);
        const wchar_t* leaf = wcsrchr(path, L'\\');
        char module[MAX_PATH]{};
        WideCharToMultiByte(CP_UTF8, 0, leaf ? leaf+1 : path, -1, module, MAX_PATH, nullptr, nullptr);
        WriteLog("sdk_first_call", std::string("\"api\":\"")+name+"\",\"caller\":\""+module+
            "\",\"sdk_internal\":"+(owner && (owner == fgOwner || owner == llOwner) ? "true" : "false"));
    } catch (...) {}
}
// These SDK-private context entry points are present in shipping Intel DLLs,
// but absent from the public headers. Native calls keep their full ABI through
// opaque tail forwarding. A tracked/retired private token gets UNSUPPORTED;
// the mod does not forward it across providers or implement private interfaces.
#define INTERNAL_API_LIST(X) \
    X(0, true, xefgSwapChainD3D12GetProfilingData) \
    X(1, true, xefgSwapChainD3D12SetDiagnosticCallbacks) \
    X(2, true, xefgSwapChainGetParameterP) \
    X(3, true, xefgSwapChainSetParameterP) \
    X(4, false, xellD3D12SetAppQueue) \
    X(5, false, xellGetContextParameterP) \
    X(6, false, xellGetLastPresentStartFrameId) \
    X(7, false, xellQueryInterface) \
    X(8, false, xellSetContextParameterP) \
    X(9, false, xellSetDisplayInfo) \
    X(10, false, xellSetFgEnabled) \
    X(11, false, xellSetGeneratedFramesCount)
#define DECLARE_GUARD(index, fg, name) extern "C" void XessInternalGuard##index();
INTERNAL_API_LIST(DECLARE_GUARD)
#undef DECLARE_GUARD
struct InternalEntry { const char* name; bool fg; void* hook; void* target{}; void* original{}; };
std::array internalEntries{
#define INTERNAL_ENTRY(index, fg, name) InternalEntry{#name, fg, reinterpret_cast<void*>(&XessInternalGuard##index)},
    INTERNAL_API_LIST(INTERNAL_ENTRY)
#undef INTERNAL_ENTRY
};
extern "C" int XessUnsupportedInternalApi() noexcept { return -10; }
extern "C" void* XessInternalApiTarget(unsigned index, void* handle, void* caller) noexcept
{
    if (index >= internalEntries.size()) return reinterpret_cast<void*>(&XessUnsupportedInternalApi);
    const bool published = prepared.load(std::memory_order_acquire);
    auto& entry=internalEntries[index];
    if (!published || !entry.original) return reinterpret_cast<void*>(&XessUnsupportedInternalApi);
    const DWORD error=GetLastError();
    void* target=entry.original;
    if (active.load(std::memory_order_acquire) && !SdkCaller(caller) && PrivateApiHandle(entry.fg,handle))
        target=reinterpret_cast<void*>(&XessUnsupportedInternalApi);
    SetLastError(error);
    return target;
}
bool OwnCode(HMODULE module, const void* code) noexcept
{
    MEMORY_BASIC_INFORMATION info{};
    if (!code || VirtualQuery(code, &info, sizeof(info)) != sizeof(info)) return false;
    const DWORD access = info.Protect & 0xff;
    return info.AllocationBase == module && info.Type == MEM_IMAGE && info.State == MEM_COMMIT
        && !(info.Protect & PAGE_GUARD) && (access == PAGE_EXECUTE || access == PAGE_EXECUTE_READ
            || access == PAGE_EXECUTE_READWRITE || access == PAGE_EXECUTE_WRITECOPY);
}
// beta.7: an entry OptiScaler redirected into itself. Upstream master 9171bf45 detours its own libxell's exports
// to its XeLL input: a 5-byte jump to a relay (jmp [rip+disp]) whose target is OptiScaler's image. Followed only
// while OptiScaler is identified; any other jump at an entry stays foreign.
bool HostRedirectedEntry(const void* entry) noexcept
{
    if (!entry || !optiscaler_host::Detected()) return false;
    const auto* code = static_cast<const unsigned char*>(entry);
    for (unsigned hop = 0; hop < 4; ++hop) {
        unsigned char bytes[6]{};
        SIZE_T copied{};
        if (!ReadProcessMemory(GetCurrentProcess(), code, bytes, sizeof(bytes), &copied) || copied != sizeof(bytes))
            return false;
        int32_t displacement{};
        if (bytes[0] == 0xe9) {
            memcpy(&displacement, bytes + 1, sizeof(displacement));
            code += 5 + displacement;
        } else if (hop && bytes[0] == 0xff && bytes[1] == 0x25) {
            memcpy(&displacement, bytes + 2, sizeof(displacement));
            uintptr_t target{};
            if (!ReadProcessMemory(GetCurrentProcess(), code + 6 + displacement, &target, sizeof(target), &copied)
                || copied != sizeof(target)) return false;
            code = reinterpret_cast<const unsigned char*>(target);
        } else return false;
        if (optiscaler_host::IsHostAddress(code)) return true;
    }
    return false;
}
bool Decode(HMODULE module, const unsigned char* code, hde64s& instruction, std::array<unsigned char,16>& bytes) noexcept
{
    if (!OwnCode(module,code)) return false;
    MEMORY_BASIC_INFORMATION memory{};
    if (VirtualQuery(code,&memory,sizeof(memory))!=sizeof(memory)) return false;
    const auto available=(std::min)(size_t(15),uintptr_t(memory.BaseAddress)+memory.RegionSize-uintptr_t(code));
    SIZE_T copied{}; bytes={};
    if (!ReadProcessMemory(GetCurrentProcess(),code,bytes.data(),available,&copied) || copied!=available) return false;
    hde64_disasm(bytes.data(),&instruction);
    return instruction.len && instruction.len<=available && !(instruction.flags & F_ERROR);
}
bool PrivateContextAbi(HMODULE module, const void* entry) noexcept
{
    // Private APIs have no published signature. Admit only an owned, decoded
    // wrapper proving RCX is the context and its null case returns SDK -8 in
    // EAX. This establishes the bounded UNSUPPORTED guard's input/result ABI;
    // no layout, argument count or implementation RVA is transferred.
    const auto* code=static_cast<const unsigned char*>(entry);
    for (unsigned hop=0; hop<4; ++hop) {
        hde64s instruction{}; std::array<unsigned char,16> bytes{};
        if (!Decode(module,code,instruction,bytes)) return false;
        if (instruction.opcode==0xe9 && instruction.len==5) code+=5+int32_t(instruction.imm.imm32);
        else if (instruction.opcode==0xeb && instruction.len==2) code+=2+int8_t(instruction.imm.imm8);
        else break;
    }
    for (unsigned count=0; count<32; ++count) {
        hde64s instruction{}; std::array<unsigned char,16> bytes{};
        if (!Decode(module,code,instruction,bytes)) return false;
        if (instruction.len==3 && bytes[0]==0x48 && bytes[1]==0x85 && bytes[2]==0xc9) {
            code+=3;
            if (!Decode(module,code,instruction,bytes)) return false;
            const unsigned char* nullCase{};
            if ((instruction.opcode==0x75 && instruction.len==2) || (instruction.opcode==0x0f && instruction.opcode2==0x85))
                nullCase=code+instruction.len;
            else if (instruction.opcode==0x74 && instruction.len==2) nullCase=code+2+int8_t(instruction.imm.imm8);
            else if (instruction.opcode==0x0f && instruction.opcode2==0x84) nullCase=code+instruction.len+int32_t(instruction.imm.imm32);
            if (!nullCase || !Decode(module,nullCase,instruction,bytes)) return false;
            return (instruction.len==5 && bytes[0]==0xb8 && instruction.imm.imm32==0xfffffff8u)
                || (instruction.len==3 && bytes[0]==0x8d && bytes[1]==0x41 && bytes[2]==0xf8);
        }
        // Only a prologue that preserves incoming RCX may precede the check.
        // In particular, a moved second argument must not masquerade as context.
        const unsigned reg=instruction.modrm_reg+(instruction.rex_r?8:0);
        const unsigned rm=instruction.modrm_rm+(instruction.rex_b?8:0);
        const bool push=instruction.opcode>=0x50 && instruction.opcode<=0x57;
        const bool store=instruction.opcode==0x89 && (instruction.modrm_mod!=3 || rm!=1);
        const bool load=(instruction.opcode==0x8b || instruction.opcode==0x8d
            || (instruction.opcode==0x0f && (instruction.opcode2==0xb6 || instruction.opcode2==0xb7
                || instruction.opcode2==0xbe || instruction.opcode2==0xbf))) && reg!=1;
        const bool immediate=instruction.opcode>=0xb8 && instruction.opcode<=0xbf
            && (instruction.opcode-0xb8+(instruction.rex_b?8:0))!=1;
        const bool stack=instruction.opcode==0x83 && instruction.modrm_mod==3 && rm==4;
        const bool nop=instruction.opcode==0x90 || (instruction.opcode==0x0f && instruction.opcode2==0x1f);
        if (!push && !store && !load && !immediate && !stack && !nop) return false;
        code+=instruction.len;
    }
    return false;
}
bool PrivateModule(HMODULE module) noexcept
{
    // The mod itself is never the game's SDK.
    return module == selfModule;
}
bool KnownExports(HMODULE module, bool fg) noexcept
{
    MODULEINFO info{};
    if (!GetModuleInformation(GetCurrentProcess(), module, &info, sizeof(info))) return false;
    auto* base = reinterpret_cast<const unsigned char*>(module);
    const auto bytes = size_t(info.SizeOfImage);
    auto fits = [&](size_t rva, size_t n) {
        if (rva >= bytes || n > bytes-rva) return false;
        MEMORY_BASIC_INFORMATION memory{};
        return VirtualQuery(base+rva,&memory,sizeof(memory))==sizeof(memory) && memory.AllocationBase==module
            && memory.Type==MEM_IMAGE && memory.State==MEM_COMMIT && !(memory.Protect & (PAGE_NOACCESS|PAGE_GUARD))
            && uintptr_t(base+rva)+n <= uintptr_t(memory.BaseAddress)+memory.RegionSize;
    };
    auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (!fits(0, sizeof(*dos)) || dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0
        || !fits(dos->e_lfanew, sizeof(IMAGE_NT_HEADERS64))) return false;
    auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base+dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE || nt->FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64
        || nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC
        || nt->OptionalHeader.NumberOfRvaAndSizes <= IMAGE_DIRECTORY_ENTRY_EXPORT) return false;
    auto dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
    if (!fits(dir.VirtualAddress, sizeof(IMAGE_EXPORT_DIRECTORY))) return false;
    auto* ex = reinterpret_cast<const IMAGE_EXPORT_DIRECTORY*>(base+dir.VirtualAddress);
    if (ex->NumberOfNames > 4096 || !fits(ex->AddressOfNames, size_t(ex->NumberOfNames)*4)) return false;
    auto* names = reinterpret_cast<const DWORD*>(base+ex->AddressOfNames);
    const char* prefix = fg ? "xefgSwapChain" : "xell";
    for (DWORD i=0; i<ex->NumberOfNames; ++i) {
        if (!fits(names[i], 1)) return false;
        const char* name = reinterpret_cast<const char*>(base+names[i]);
        const auto limit = (std::min)(size_t(256), bytes-names[i]);
        if (!fits(names[i],limit) || !memchr(name, 0, limit)) return false;
        if (strncmp(name, prefix, strlen(prefix))) continue;
        if (!strcmp(name, fg ? "xefgSwapChainGetVersion" : "xellGetVersion")) continue;
        if (!fg && (!strcmp(name,"xellAILGetDecision") || !strcmp(name,"xellAILGetVersion")
            || !strcmp(name,"xellAILIsSupportedDevice"))) continue;
        bool known{};
        for (const auto& entry : entries) if (entry.fg == fg && !strcmp(name, entry.name)) known = true;
        for (const auto& entry : internalEntries) if (entry.fg == fg && !strcmp(name, entry.name)) known = true;
        if (!known) return false;
    }
    return true;
}
bool Candidate(HMODULE module, bool fg) noexcept
{
    const auto version = RawProc(module, fg ? "xefgSwapChainGetVersion" : "xellGetVersion");
    if (!OwnCode(module, reinterpret_cast<void*>(version))) return false;
    uint16_t major{}, minor{}, patch{};
    if (fg) {
        xefg_swapchain_version_t v{};
        if (reinterpret_cast<decltype(&xefgSwapChainGetVersion)>(version)(&v) != 0) return false;
        major=v.major; minor=v.minor; patch=v.patch;
    } else {
        xell_version_t v{};
        if (reinterpret_cast<decltype(&xellGetVersion)>(version)(&v) != 0) return false;
        major=v.major; minor=v.minor; patch=v.patch;
    }
    if (major != 1 || !KnownExports(module, fg)) return false;
    size_t index{};
    for (const auto& entry : entries) {
        if (entry.fg != fg) continue;
        const auto address = RawProc(module, entry.name);
        if ((!address && index < (fg ? 11u : 6u)) || (address && !OwnCode(module, reinterpret_cast<void*>(address)))) return false;
        ++index;
    }
    for (const auto& entry : internalEntries) {
        if (entry.fg != fg) continue;
        const auto address=RawProc(module,entry.name);
        // beta.7: OptiScaler's own entry jump hides the wrapper; that entry is its, not ours to guard
        if (address && !HostRedirectedEntry(reinterpret_cast<void*>(address))
            && !PrivateContextAbi(module,reinterpret_cast<void*>(address))) return false;
    }
    return true;
}
}
HMODULE Self() noexcept { return selfReady.load(std::memory_order_acquire) ? selfModule : nullptr; }
HMODULE NativeFgModule() noexcept { return Active() ? fgOwner : nullptr; }
HMODULE NativeLlModule() noexcept { return Active() ? llOwner : nullptr; }
bool HostOwnsLatency() noexcept { return Active() && hostLatency; }
FARPROC RawSdkExport(HMODULE module, LPCSTR name) noexcept
{
    if (!module || !name || uintptr_t(name) <= 0xffff) return nullptr;
    return RawProc(module, name);
}

bool ResolveNativeSdkCreate(HMODULE importer, const void* caller, HMODULE module,
    LPCSTR name, FARPROC& result) noexcept
{
    const DWORD incomingError = GetLastError();
    const bool createName = name && reinterpret_cast<uintptr_t>(name) > 0xffff
        && (!strcmp(name, "xefgSwapChainD3D12CreateContext") || !strcmp(name, "xellD3D12CreateContext"));
    // The selected owner pair is immutable after active's release publication.
    // Do not inspect it before acquire, or trust a stolen wrapper's importer.
    const bool handled = createName && module && importer && active.load(std::memory_order_acquire)
        && (importer == fgOwner || importer == llOwner)
        && OwnCode(importer, caller);
    SetLastError(incomingError);
    if (!handled) return false;
    result = RawProc(module, name);
    return true;
}
bool Active() noexcept { return active.load(std::memory_order_acquire); }
bool InstallImports(HMODULE module) noexcept
{
    std::lock_guard lock(discoveryMutex);
    if (selfReady.load(std::memory_order_acquire))
        return module == selfModule;
    HMODULE pinned{};
    if (!module || (selfModule && selfModule != module)
        || !GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
            reinterpret_cast<LPCWSTR>(module), &pinned) || pinned != module) return false;
    selfModule = module;
    selfReady.store(true, std::memory_order_release);
    return true;
}
// OptiScaler loads its own XeSS FG/XeLL pair (v10: <host dir>\OptiScaler\,
// 0.9.4: beside the host) while the game may load its own copies. When a role
// has several candidates, the copy in the detected host's library folder is
// the one OptiScaler drives (it also redirects the game's XeLL exports to its
// own copy). Identity is still established later by the exact provider
// profile; the folder only selects between otherwise-admitted candidates.
bool InDirectory(HMODULE module, const std::wstring& directory) noexcept
{
    wchar_t path[MAX_PATH]{};
    const DWORD length = GetModuleFileNameW(module, path, MAX_PATH);
    if (!length || length >= MAX_PATH || directory.empty() || length <= directory.size()) return false;
    if (_wcsnicmp(path, directory.c_str(), directory.size()) != 0) return false;
    return !wcschr(path + directory.size(), L'\\');
}
HMODULE PreferHostCopy(const std::vector<HMODULE>& candidates) noexcept
{
    if (!optiscaler_host::Detected()) return nullptr;
    const auto host = optiscaler_host::Read();
    std::wstring directory(host.path);
    const size_t slash = directory.find_last_of(L'\\');
    if (slash == std::wstring::npos) return nullptr;
    directory.resize(slash + 1);
    for (const std::wstring& folder : {directory + L"OptiScaler\\", directory}) {
        HMODULE chosen{};
        size_t matches{};
        for (HMODULE module : candidates) if (InDirectory(module, folder)) { chosen = module; ++matches; }
        if (matches == 1) return chosen;
        if (matches > 1) return nullptr;
    }
    return nullptr;
}
void DiscoverSdkModules() noexcept
{
    std::lock_guard lock(discoveryMutex);
    if (!selfReady.load(std::memory_order_acquire)) return;
    // beta.8: XeSS MFG runs under OptiScaler only. Without it a game's own XeSS-FG stays untouched:
    // no resolver detour, no candidate pins, no SDK hooks. Nothing latches, so OptiScaler loaded
    // later still activates the route (contexts created before then stay native).
    if (!optiscaler_host::Detected())
    {
        static std::atomic<bool> logged{};
        if (!logged.exchange(true, std::memory_order_relaxed))
            WriteLog("sdk_route_requires_optiscaler", "\"optiscaler\":false,\"native_xefg\":\"untouched\"");
        return;
    }
    InstallResolver();
    if (attempted) return;
    try {
        std::array<HMODULE, 2048> modules{};
        DWORD bytes{};
        if (!EnumProcessModulesEx(GetCurrentProcess(), modules.data(), sizeof(modules), &bytes, LIST_MODULES_64BIT)
            || bytes > sizeof(modules)) return;
        std::vector<HMODULE> fgs, lls;
        for (size_t i=0; i<bytes/sizeof(HMODULE); ++i) {
            const auto module = modules[i];
            if (PrivateModule(module)) continue;
            HMODULE retained{};
            if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                reinterpret_cast<LPCWSTR>(module), &retained) || retained != module) continue;
            const bool isFg = Candidate(module, true), isLl = Candidate(module, false);
            if (isFg || isLl) {
                HMODULE pinned{};
                if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                    reinterpret_cast<LPCWSTR>(module), &pinned) || pinned != module) { FreeLibrary(retained); return; }
            }
            if (isFg && std::find(fgs.begin(), fgs.end(), module) == fgs.end()) fgs.push_back(module);
            if (isLl && std::find(lls.begin(), lls.end(), module) == lls.end()) lls.push_back(module);
            FreeLibrary(retained);
        }
        const auto choose = [](const std::vector<HMODULE>& list, const char* role) -> HMODULE {
            if (list.size() == 1) return list.front();
            if (list.size() < 2) return nullptr;
            HMODULE preferred = PreferHostCopy(list);
            WriteLog("sdk_candidates_ambiguous", std::string("\"role\":\"") + role + "\",\"count\":"
                + std::to_string(list.size()) + ",\"optiscaler_copy\":" + (preferred ? "true" : "false"));
            return preferred;
        };
        const HMODULE fg = choose(fgs, "fg"), ll = choose(lls, "ll");
        // Activate one complete pair. Additional providers retain native contexts.
        if (!fg || !ll || fg == ll) return;
        attempted = true;
        // RTXMFG's DllMain already owns the per-process module claim; DLSS-G
        // and XeSS-FG are two backends of the same owner.
        auto init = MH_Initialize();
        if (init != MH_OK && init != MH_ERROR_ALREADY_INITIALIZED) return;
        // Witness the pristine SDK before our public-entry detours change its thunks.
        // A private-layout rejection does not disable the native public API route.
        // beta.7: entry jumps OptiScaler placed into itself don't hide the layout behind them.
        xess_layout::entryRedirectFilter.store(&HostRedirectedEntry, std::memory_order_release);
        const bool privateLayout = xess_layout::Pair(fg, ll);
        WriteLog("sdk_private_layout", std::string("\"eligible\":") + (privateLayout ? "true" : "false"));
        fgOwner = fg; llOwner = ll;
        std::vector<void*> created;
        size_t hostRedirected{}, hostLifetime{};
        for (auto& entry : entries) {
            entry.target = reinterpret_cast<void*>(RawProc(entry.fg ? fg : ll, entry.name));
            if (!entry.target) continue;
            // beta.7: OptiScaler routes this export itself (its XeLL input). Calls keep taking its jump, which
            // gets no second hook on top.
            if (HostRedirectedEntry(entry.target)) {
                *entry.original = entry.target; ++hostRedirected;
                if (!entry.fg && (!strcmp(entry.name, "xellD3D12CreateContext") || !strcmp(entry.name, "xellDestroyContext")))
                    ++hostLifetime;
                continue;
            }
            void* trampoline{};
            if (MH_CreateHook(entry.target, entry.hook, &trampoline) != MH_OK) {
                for (void* target : created) MH_RemoveHook(target);
                WriteLog("sdk_route_rejected", "\"reason\":\"api_hook_plan_failed\""); return;
            }
            *entry.original = trampoline;
            created.push_back(entry.target);
        }
        for (auto& entry : internalEntries) {
            entry.target = reinterpret_cast<void*>(RawProc(entry.fg ? fg : ll,entry.name));
            if (!entry.target) continue;
            if (HostRedirectedEntry(entry.target)) { entry.original = entry.target; ++hostRedirected; continue; }
            if (MH_CreateHook(entry.target,entry.hook,&entry.original) != MH_OK) {
                for (void* target : created) MH_RemoveHook(target);
                WriteLog("sdk_route_rejected","\"reason\":\"private_api_guard_plan_failed\""); return;
            }
            created.push_back(entry.target);
        }
        hostLatency = hostLifetime == 2;
        prepared.store(true, std::memory_order_release);
        bool queued = true;
        for (void* target : created) if (MH_QueueEnableHook(target) != MH_OK) queued = false;
        const bool enabled = queued && MH_ApplyQueued() == MH_OK;
        // Never remove trampolines after publication, even on partial enablement.
        if (!enabled) { WriteLog("sdk_route_rejected", "\"reason\":\"api_hook_commit_failed_native_only\""); return; }
        active.store(true, std::memory_order_release);
        LogSdkModule(fg,"fg"); LogSdkModule(ll,"ll");
        WriteLog("sdk_route_ready", "\"entrypoints\":"+std::to_string(created.size())+
            ",\"fg_module\":"+std::to_string(uintptr_t(fg))+",\"ll_module\":"+std::to_string(uintptr_t(ll))+
            ",\"host_redirected\":"+std::to_string(hostRedirected)+",\"host_xell\":"+(hostLatency ? "true" : "false"));
    } catch (...) { WriteLog("sdk_route_rejected", "\"reason\":\"discovery_exception\""); }
}
}
