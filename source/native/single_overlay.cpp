#include "single_overlay.h"
#include "overlay_build.h"
#include "overlay_install.h"
#include "overlay_platform.h"
#include "overlay_dxgi_proxy.h"
#include "overlay_vulkan.h"
#include "overlay_native.h"
#include "overlay_application_imports.h"
#include "overlay_adapter_parent.h"
#include "caller_scoped_import.h"
#include "witcher_dots/witcher_dots.h"
#include <d3d12.h>
#include <intrin.h>
#include <atomic>
#include <array>
#include <cstring>
#include <memory>
#include <new>
#include <type_traits>
#include <utility>

namespace single_overlay {
namespace {
// Bindings have process lifetime and are published before a gateway escapes.
// A distinct original entry always gets a distinct typed gateway.
// First call through each gateway: which export it binds and who called it.
// Distinguishes application, middleware and scoped-entry routes in game logs.
void LogFirstGatewayCall(const char* api,FARPROC original,void* caller) noexcept {
    std::unique_ptr<wchar_t[]> storage(new(std::nothrow) wchar_t[2*32768+512]);
    if (!storage) return;
    wchar_t* owner=storage.get();
    wchar_t* from=owner+32768;
    wchar_t* line=from+32768;
    auto leaf=[](const void* address,wchar_t* path,uintptr_t& rva) {
        HMODULE module=nullptr;
        rva=0;
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                reinterpret_cast<LPCWSTR>(address),&module) || !module) { wcscpy_s(path,32768,L"(none)"); return path; }
        const DWORD n=GetModuleFileNameW(module,path,32768);
        if (!n || n>=32768) { wcscpy_s(path,32768,L"(unnamed)"); return path; }
        rva=reinterpret_cast<uintptr_t>(address)-reinterpret_cast<uintptr_t>(module);
        const wchar_t* separator=wcsrchr(path,L'\\');
        return separator?const_cast<wchar_t*>(separator+1):path;
    };
    uintptr_t ownerRva=0,callerRva=0;
    const wchar_t* ownerLeaf=leaf(reinterpret_cast<const void*>(original),owner,ownerRva);
    const wchar_t* callerLeaf=leaf(caller,from,callerRva);
    swprintf_s(line,512,L"MFG_PROXY_UI factory gateway first call api=%hs entry=%s caller=%s+0x%llX",
        api,ownerLeaf,callerLeaf,static_cast<unsigned long long>(callerRva));
    single_module::Log(line);
}
template<class Tag,class Fn> struct Gateways {
    // Keep entry identity separate from a published trampoline. Repeated slInit
    // calls must reuse the same gateway after a scoped entry has been armed.
    inline static std::array<std::atomic<FARPROC>,16> originals{};
    inline static std::array<std::atomic<FARPROC>,16> trampolines{};
    inline static std::array<std::atomic<bool>,16> called{};
    template<size_t I> static FARPROC Original() noexcept {
        if (const auto trampoline=trampolines[I].load(std::memory_order_acquire)) return trampoline;
        return originals[I].load(std::memory_order_acquire);
    }
    template<size_t I> static HRESULT WINAPI Factory(REFIID iid,void** output) {
        if (!called[I].exchange(true,std::memory_order_acq_rel))
            LogFirstGatewayCall(Tag::name,originals[I].load(std::memory_order_acquire),_ReturnAddress());
        return proxy::FactoryCall(reinterpret_cast<proxy::FactoryFn>(Original<I>()),iid,output);
    }
    template<size_t I> static HRESULT WINAPI Factory2(UINT flags,REFIID iid,void** output) {
        if (!called[I].exchange(true,std::memory_order_acq_rel))
            LogFirstGatewayCall(Tag::name,originals[I].load(std::memory_order_acquire),_ReturnAddress());
        return proxy::FactoryCall(reinterpret_cast<proxy::Factory2Fn>(Original<I>()),flags,iid,output);
    }
    template<size_t... I> static auto Entries(std::index_sequence<I...>) {
        if constexpr (std::is_same_v<Fn,proxy::Factory2Fn>) return std::array<FARPROC,sizeof...(I)>{reinterpret_cast<FARPROC>(&Factory2<I>)...};
        else return std::array<FARPROC,sizeof...(I)>{reinterpret_cast<FARPROC>(&Factory<I>)...};
    }
    inline static const auto entries=Entries(std::make_index_sequence<16>{});
    static bool PublishOriginal(void* target,void* trampoline) noexcept {
        if (!target||!trampoline) return false;
        for (size_t i=0;i<originals.size();++i) if (originals[i].load(std::memory_order_acquire)==reinterpret_cast<FARPROC>(target)) {
            FARPROC expected=nullptr;
            return trampolines[i].compare_exchange_strong(expected,reinterpret_cast<FARPROC>(trampoline),std::memory_order_acq_rel)
                || expected==reinterpret_cast<FARPROC>(trampoline);
        }
        return false;
    }
    static FARPROC Bind(FARPROC target) noexcept {
        for (size_t i=0;i<originals.size();++i) {
            if (entries[i]==target) return target;
            FARPROC expected=nullptr;
            if (originals[i].compare_exchange_strong(expected,target,std::memory_order_acq_rel)||expected==target) return entries[i];
        }
        single_module::Log(L"MFG_PROXY_UI factory gateway capacity exhausted; original entry retained");
        return target;
    }
};
struct Factory0 { static constexpr const char* name="CreateDXGIFactory"; };
struct Factory1 { static constexpr const char* name="CreateDXGIFactory1"; };
struct Factory2 { static constexpr const char* name="CreateDXGIFactory2"; };
using DeviceFn=HRESULT(WINAPI*)(IUnknown*,D3D_FEATURE_LEVEL,REFIID,void**);
struct DeviceGateways {
    inline static std::array<std::atomic<DeviceFn>,8> originals{};
    template<size_t I> static HRESULT WINAPI Create(IUnknown* adapter,D3D_FEATURE_LEVEL level,REFIID iid,void** output) {
        adapter_parent::Observe(adapter);
        if(output)witcher_dots::BeforeDeviceCreate(_ReturnAddress());
        const auto result=originals[I].load(std::memory_order_acquire)(adapter,level,iid,output);
        const DWORD error=GetLastError();
        if(SUCCEEDED(result)&&output&&*output)
            witcher_dots::ObserveDevice(static_cast<IUnknown*>(*output),_ReturnAddress());
        SetLastError(error);
        return result;
    }
    template<size_t... I> static auto Entries(std::index_sequence<I...>) {
        return std::array<DeviceFn,sizeof...(I)>{&Create<I>...};
    }
    static FARPROC Bind(FARPROC entry) noexcept {
        static const auto entries=Entries(std::make_index_sequence<8>{});
        const auto target=reinterpret_cast<DeviceFn>(entry);
        for(size_t i=0;i<originals.size();++i) {
            if(entries[i]==target)return entry;
            DeviceFn expected=nullptr;
            if(originals[i].compare_exchange_strong(expected,target,std::memory_order_acq_rel)||expected==target)
                return reinterpret_cast<FARPROC>(entries[i]);
        }
        return entry;
    }
};
bool PublishFactoryOriginal(const char* name,void* target,void* trampoline) noexcept {
    if (!strcmp(name,"CreateDXGIFactory")) return Gateways<Factory0,proxy::FactoryFn>::PublishOriginal(target,trampoline);
    if (!strcmp(name,"CreateDXGIFactory1")) return Gateways<Factory1,proxy::FactoryFn>::PublishOriginal(target,trampoline);
    if (!strcmp(name,"CreateDXGIFactory2")) return Gateways<Factory2,proxy::Factory2Fn>::PublishOriginal(target,trampoline);
    return false;
}
bool Eligible(HMODULE module,FARPROC original) noexcept {
    // Factory resolution may be called from small-stack engine workers too.
    std::unique_ptr<wchar_t[]> storage(new(std::nothrow) wchar_t[32768]);
    if (!storage) return false;
    wchar_t* path=storage.get();
    wchar_t system[MAX_PATH]{};
    const DWORD count=module?GetModuleFileNameW(module,path,32768):0;
    if (!count||count>=32768||!slots::ImageEntry(module,reinterpret_cast<void*>(original))) return false;
    const wchar_t* leaf=wcsrchr(path,L'\\'); leaf=leaf?leaf+1:path;
    bool eligible=false;
    if (!_wcsicmp(leaf,L"dxgi.dll")) {
        if (!GetSystemDirectoryW(system,MAX_PATH)||wcscat_s(system,L"\\dxgi.dll")) return false;
        eligible=!_wcsicmp(path,system);
        if (!eligible) {
            // Local ReShade DXGI exposes the same public factory ABI. Bind
            // only entries owned by this image; never patch its COM methods.
            eligible=true;
            for (const char* name:{"CreateDXGIFactory","CreateDXGIFactory1","CreateDXGIFactory2",
                                  "ReShadeRegisterAddon","ReShadeUnregisterAddon"})
                eligible=slots::ImageEntry(module,reinterpret_cast<void*>(GetProcAddress(module,name)))&&eligible;
        }
    } else if (!_wcsicmp(leaf,L"d3d12.dll")) {
        if (!GetSystemDirectoryW(system,MAX_PATH)||wcscat_s(system,L"\\d3d12.dll")) return false;
        eligible=!_wcsicmp(path,system)
            && GetProcAddress(module,"D3D12CreateDevice")==original;
        if (!eligible && witcher_dots::GameProcess()) {
            // Witcher DOTS must observe the renderer's device even through a
            // local ReShade d3d12.dll (same public ABI); it resolves the native
            // device behind the returned wrapper itself.
            eligible=GetProcAddress(module,"D3D12CreateDevice")==original;
            for (const char* name:{"ReShadeRegisterAddon","ReShadeUnregisterAddon"})
                eligible=slots::ImageEntry(module,reinterpret_cast<void*>(GetProcAddress(module,name)))&&eligible;
        }
    } else if (!_wcsicmp(leaf,L"sl.interposer.dll")) {
        // Establish the expected public interposer export family in this exact
        // loaded image. Never bind a shared/private Streamline COM method.
        eligible=true;
        for (const char* name:{"slInit","slSetD3DDevice","CreateDXGIFactory1","CreateDXGIFactory2"})
            eligible=slots::ImageEntry(module,reinterpret_cast<void*>(GetProcAddress(module,name)))&&eligible;
    }
    HMODULE pinned=nullptr;
    return eligible&&GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_PIN,
        reinterpret_cast<LPCWSTR>(original),&pinned)&&pinned==module;
}
// Where Winds Meet's packed executable resolves the interposer's factory
// exports itself: no import slot or GetProcAddress call reaches us, so only
// Streamline's swapchain is created. Route main-executable callers of this
// interposer's public factory entries through the same typed gateways.
// Streamline, middleware and RTXMFG callers keep the original entry.
void ArmInterposerFactories() noexcept {
    const HMODULE interposer=GetModuleHandleW(L"sl.interposer.dll");
    if (!interposer) return;
    for (const char* name:{"CreateDXGIFactory","CreateDXGIFactory1","CreateDXGIFactory2"}) {
        const FARPROC entry=GetProcAddress(interposer,name);
        if (!entry) continue;
        const FARPROC replacement=ResolveProc(interposer,name,entry);
        if (replacement==entry) continue;
        void* trampoline=nullptr;
        if (!caller_scoped_import::PrepareInterposerEntry(reinterpret_cast<void*>(entry),reinterpret_cast<void*>(replacement),name,trampoline)
            || !PublishFactoryOriginal(name,reinterpret_cast<void*>(entry),trampoline)
            || !caller_scoped_import::Activate(reinterpret_cast<void*>(entry),reinterpret_cast<void*>(replacement))) {
            wchar_t line[160]{};
            swprintf_s(line,L"MFG_PROXY_UI interposer-entry inactive symbol=%hs; other factory routes retained",name);
            single_module::Log(line);
        }
    }
}
}
FARPROC ResolveProc(HMODULE module,LPCSTR name,FARPROC original) noexcept {
    if (gInsideOverlay||!single_module::OwnsBackend()||!original||!name||reinterpret_cast<uintptr_t>(name)<=0xffff) return original;
#if MFG_UNLOCK_DIAGNOSTIC_NO_SINGLE_OVERLAY
    return original;
#else
    if (!strcmp(name,"D3D12CreateDevice")) {
        if (!Eligible(module,original)||GetProcAddress(module,name)!=original) return original;
        return DeviceGateways::Bind(original);
    }
    if (!strcmp(name,"CreateDXGIFactory")||!strcmp(name,"CreateDXGIFactory1")||!strcmp(name,"CreateDXGIFactory2")) {
        if (!Eligible(module,original)) return original;
        if (!strcmp(name,"CreateDXGIFactory")) return Gateways<Factory0,proxy::FactoryFn>::Bind(original);
        if (!strcmp(name,"CreateDXGIFactory1")) return Gateways<Factory1,proxy::FactoryFn>::Bind(original);
        return Gateways<Factory2,proxy::Factory2Fn>::Bind(original);
    }
#if MFG_UNLOCK_OVERLAY_MENU_DRAW
    if (name[0]=='v' && name[1]=='k') return vulkan::Resolve(module,name,original);
    return ResolveBoundedInput(module,name,original);
#else
    return original;
#endif
#endif
}
bool OwnsInterface(void* object) noexcept {
    return object&&!gInsideOverlay&&proxy::Owned(static_cast<IUnknown*>(object));
}
bool WrapUpgradedInterface(void** output) noexcept {
    if (gInsideOverlay||!single_module::OwnsBackend()) return false;
#if MFG_UNLOCK_DIAGNOSTIC_NO_SINGLE_OVERLAY
    (void)output;
    return false;
#else
    return proxy::WrapUpgraded(output);
#endif
}
void ArmFactoryGateway() noexcept {
    static std::atomic_flag publishing=ATOMIC_FLAG_INIT;
    if(publishing.test_and_set(std::memory_order_acquire))return;
    struct ReleasePublication {std::atomic_flag& value;~ReleasePublication(){value.clear(std::memory_order_release);}} release{publishing};
    HMODULE executable=GetModuleHandleW(nullptr);
    slots::Batch batch;
    const bool loaderCallout=native::InsideLoader();
    const bool dotsDevice=witcher_dots::GameProcess();
    size_t deferred=0;
    const bool inspected=slots::VisitImports(executable,[&](const char* name,void** slot) {
        if (strcmp(name,"CreateDXGIFactory")&&strcmp(name,"CreateDXGIFactory1")&&strcmp(name,"CreateDXGIFactory2")
            && !(dotsDevice&&!strcmp(name,"D3D12CreateDevice"))
            && !(name[0]=='v'&&name[1]=='k')) return true;
        void* value=protected_pointer::ReadPointer(reinterpret_cast<uintptr_t>(slot));
        HMODULE owner=nullptr;
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(value),&owner)) return false;
        const auto original=reinterpret_cast<FARPROC>(value);
        const auto replacement=ResolveProc(owner,name,original);
        if (original==replacement) return true;
        if(!strcmp(name,"D3D12CreateDevice")) {
            // DOTS requires this early, aligned import. Do not use the
            // factory-entry fallback for a different public ABI.
            if(!slots::ImageSlot(executable,slot)) {++deferred;return true;}
            return batch.Add(executable,slot,value,reinterpret_cast<void*>(replacement));
        }
        // DllMain retains only aligned data publication. Public-entry fallback
        // is retried synchronously at slInit, before application graphics calls.
        if (loaderCallout && !slots::ImageSlot(executable,slot)) { ++deferred; return true; }
        return batch.Add(executable,slot,value,reinterpret_cast<void*>(replacement),name,&PublishFactoryOriginal);
    });
    if (!inspected||!batch.Publish()) single_module::Log(L"MFG_PROXY_UI application graphics import publication unavailable; dynamic gateways remain available");
    wchar_t line[160]{};
    swprintf_s(line,L"MFG_PROXY_UI armed mainGraphicsImports=%zu graphicsProbes=0 nativeTableWrites=0",batch.published);
    single_module::Log(line);
    if (deferred) single_module::Log(L"MFG_PROXY_UI unaligned graphics imports deferred to the pre-slInit boundary");
    // Entry relays need MinHook; never from a loader callout (DllMain).
    if (!loaderCallout) ArmInterposerFactories();
    application_imports::ArmStartupDependencies();
}
void InstallKnownModules() noexcept {
    if (single_module::OwnsBackend()) install::RequestInstall();
}
void BeforeStreamlineInit() noexcept {
    // Existing early boundary: aligned application imports or caller-scoped
    // public DXGI entries. No graphics creation or native COM table mutation.
    ArmFactoryGateway();
}
}
