// DXGI interface signatures follow the public Windows API. Architecture was
// informed by ReShade 6.8.0 (Patrick Mours, BSD-3-Clause OR MIT): create first,
// then own a proxy with an explicit lifetime. No ReShade code is linked.
#include "overlay_dxgi_proxy.h"
#include "overlay_native.h"
#include "overlay_dx12.h"
#include "overlay_install.h"
#include "overlay_platform.h"
#include "vsync_control.h"
#include <d3d12.h>
#include <intrin.h>
#include <mutex>
#if MFG_UNLOCK_XESS
#include "xess/xess_mod.h"
#include "xess/d3d12_host.h"
#endif
#include <atomic>
#include <new>
#include <array>
#include <intrin.h>

namespace single_overlay::proxy {
using Microsoft::WRL::ComPtr;
namespace {
std::atomic<uint64_t> factories{0}, chains{0}, calls{0}, skipped{0};
std::atomic<uint32_t> liveFactories{0}, liveChains{0};
struct Capacity {
    std::atomic<uint32_t>& live;
    bool held=false;
    explicit Capacity(std::atomic<uint32_t>& value):live(value) {
        uint32_t count=live.load(std::memory_order_relaxed);
        while(count<64)if(live.compare_exchange_weak(count,count+1,std::memory_order_acq_rel)){held=true;break;}
    }
    ~Capacity(){if(held)--live;}
    void Transfer(){held=false;}
};
thread_local uint32_t creationDepth = 0;
struct Creation {
    bool outer = creationDepth++ == 0 && !gInsideOverlay && !native::InsideLoader();
    ~Creation() { --creationDepth; }
};
// xefgmenu.1: a swapchain creation through a factory proxy, whose return gets
// the menu; a frame generator's output chain created inside it does not.
thread_local uint32_t swapchainCreations = 0;
struct SwapchainCreation : Creation {
    SwapchainCreation() { if (outer) ++swapchainCreations; }
    ~SwapchainCreation() { if (outer) --swapchainCreations; }
};
template<class T> struct Lease {
    T* object;
    explicit Lease(T* p) noexcept : object(p) { object->AddRef(); }
    ~Lease() { object->Release(); }
};
constexpr GUID factoryId = {0x95f71528,0xa89b,0x4d8a,{0xa3,0x15,0x60,0xca,0x09,0x12,0x10,0x01}};
constexpr GUID chainId = {0x95f71528,0xa89b,0x4d8a,{0xa3,0x15,0x60,0xca,0x09,0x12,0x10,0x02}};
constexpr IID factoryIids[] = {__uuidof(IDXGIFactory),__uuidof(IDXGIFactory1),__uuidof(IDXGIFactory2),
    __uuidof(IDXGIFactory3),__uuidof(IDXGIFactory4),__uuidof(IDXGIFactory5),__uuidof(IDXGIFactory6),__uuidof(IDXGIFactory7)};
constexpr IID chainIids[] = {__uuidof(IDXGISwapChain),__uuidof(IDXGISwapChain1),__uuidof(IDXGISwapChain2),
    __uuidof(IDXGISwapChain3),__uuidof(IDXGISwapChain4)};
constexpr size_t factoryLast[] = {11,13,24,25,27,28,29,31};
constexpr size_t chainLast[] = {17,28,35,39,40};
template<size_t N> int Version(REFIID iid, const IID (&known)[N]) noexcept {
    for (size_t i=0;i<N;++i) if (iid == known[i]) return static_cast<int>(i);
    return -1;
}

// Cache only interfaces actually requested by the application. Querying every
// factory revision speculatively can itself replace a native table after an
// existing overlay has initialized it. Each published interface owns a stable
// reference and is never changed while proxy methods can access it.
template<class Base,size_t N> class InterfaceCache {
    Base* root;
    const unsigned rootVersion;
    std::array<std::atomic<Base*>,N> values{};
public:
    InterfaceCache(Base* p,unsigned v):root(p),rootVersion(v){values[v].store(p);}
    ~InterfaceCache(){Clear();}
    void Clear() noexcept {
        for(size_t i=0;i<N;++i){auto* p=values[i].exchange(nullptr);if(p&&i!=rootVersion)p->Release();}
    }
    Base* Find(unsigned v) const noexcept {
        for(size_t i=v;i<N;++i)if(auto* p=values[i].load(std::memory_order_acquire))return p;
        return nullptr;
    }
    bool Acquire(unsigned v,REFIID iid,size_t last) noexcept {
        if(Find(v))return true;
        void* queried=nullptr;
        const HRESULT hr=root->QueryInterface(iid,&queried);
        if(hr!=S_OK||!queried){if(queried)static_cast<IUnknown*>(queried)->Release();return false;}
        auto* pointer=static_cast<Base*>(queried);
        if(!native::PinInterface(pointer,last)){pointer->Release();return false;}
        Base* expected=nullptr;
        if(!values[v].compare_exchange_strong(expected,pointer,std::memory_order_acq_rel))pointer->Release();
        return true;
    }
    template<class T> T* As(unsigned version) const noexcept {return static_cast<T*>(Find(version));}
};
template<class T> bool AlreadyWrapped(T* object, REFIID iid) noexcept {
    ComPtr<IUnknown> own;
    return object->QueryInterface(iid, reinterpret_cast<void**>(own.GetAddressOf())) == S_OK && own;
}
// diag171.1: which layers a swap-chain creation passed through. "name+0xRVA"
// for an address inside a module image, "(none)" outside one.
void ModuleAt(const void* address,wchar_t* text,size_t count) noexcept {
    HMODULE module=nullptr;
    if (!address||!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(address),&module)||!module) { swprintf_s(text,count,L"(none)"); return; }
    wchar_t path[1024]{};
    const DWORD n=GetModuleFileNameW(module,path,1024);
    const wchar_t* leaf=n&&n<1024?wcsrchr(path,L'\\'):nullptr;
    swprintf_s(text,count,L"%s+0x%llX",leaf?leaf+1:L"(unnamed)",
        static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(address)-reinterpret_cast<uintptr_t>(module)));
}
std::atomic<uint64_t> creationsLogged{0};
constexpr uint64_t kCreationLines=16;
void LogSwapchainCreation(const char* method,void* caller,IUnknown* below,HRESULT hr,IUnknown* chain,bool outer) noexcept {
    const uint64_t line=creationsLogged.fetch_add(1,std::memory_order_relaxed)+1;
    if (line>kCreationLines) return;
    wchar_t from[160]{},factory[224]{},created[224]{},text[900]{};
    ModuleAt(caller,from,160);
    Describe(below,factory,224);
    if (SUCCEEDED(hr)) Describe(chain,created,224); else swprintf_s(created,224,L"(failed)");
    swprintf_s(text,L"MFG_PROXY_UI swapchain creation %llu: method=%hs hr=0x%08lX outer=%u caller=%s factoryBelow={%s} chain={%s}",
        static_cast<unsigned long long>(line),method,static_cast<unsigned long>(hr),static_cast<unsigned>(outer),from,factory,created);
    single_module::Log(text);
}

void WrapSwapchain(IDXGISwapChain** result, IUnknown* queue, IDXGIFactory* parent) noexcept;
#if MFG_UNLOCK_XESS
bool WrapHostSwapchain(IDXGISwapChain** result, IUnknown* queue, IDXGIFactory* parent) noexcept;
#endif
class FactoryProxy final : public IDXGIFactory7 {
public:
    ComPtr<IDXGIFactory> original;
    InterfaceCache<IDXGIFactory,8> interfaces;
    const unsigned version;
    std::atomic<ULONG> refs{1};
    FactoryProxy(ComPtr<IDXGIFactory>&& base, unsigned v) noexcept : original(std::move(base)),interfaces(original.Get(),v),version(v) {
        ++factories;
    }
    ~FactoryProxy() { --liveFactories; }
    ULONG STDMETHODCALLTYPE AddRef() override { return refs.fetch_add(1,std::memory_order_relaxed)+1; }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG left=refs.fetch_sub(1,std::memory_order_acq_rel)-1;
        if (!left) delete this;
        return left;
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid,void** output) override {
        if (!output) return E_POINTER;
        Lease lease(this);
        const int v=Version(iid,factoryIids);
        if (iid==__uuidof(IUnknown)||iid==__uuidof(IDXGIObject)||iid==factoryId||(v>=0&&interfaces.Acquire(unsigned(v),iid,factoryLast[v]))) {
            *output=static_cast<IDXGIFactory7*>(this); AddRef(); return S_OK;
        }
        // Preserve unknown extension contracts. Only supported DXGI interfaces
        // are advertised by the proxy; no private object layouts are inferred.
        return original->QueryInterface(iid,output);
    }
    template<class T> T* Revision(unsigned v) noexcept {
        // Witcher 3 calls newer methods on its IDXGIFactory creation pointer
        // without QueryInterface. Acquire only the revision actually used;
        // never assume the root pointer exposes that revision's vtable.
        return interfaces.Acquire(v,factoryIids[v],factoryLast[v])?interfaces.As<T>(v):nullptr;
    }
#if MFG_UNLOCK_XESS
    // xemfgsa.1: XeMFG standalone. The host creates the game's chain on XeFG; the menu's proxy must wrap it (its
    // presents and its release are the host's frames and teardown), else the host gives it up and the game's chain is
    // created natively.
    bool HostCreate(IUnknown* q,HWND w,const DXGI_SWAP_CHAIN_DESC1& d,const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* f,
        IDXGIOutput* o,void* caller,IDXGISwapChain** out,HRESULT& hr) noexcept {
        IDXGISwapChain1* created=nullptr;
        // Asked while off as well: the host notes a chain created before the switch (restart needed).
        if (!xess_d3d12::CreateSwapChain(q,w,&d,f,o,caller,&created,hr)||!created) return false;
        IDXGISwapChain* chain=created;
        if (!WrapHostSwapchain(&chain,q,this)) { xess_d3d12::Abandon(created); return false; }
        *out=chain;
        return true;
    }
#endif
    HRESULT STDMETHODCALLTYPE CreateSwapChain(IUnknown* q,DXGI_SWAP_CHAIN_DESC* d,IDXGISwapChain** out) override {
        Lease lease(this); SwapchainCreation creation;
#if MFG_UNLOCK_XESS
        if (creation.outer&&d&&out) {
            DXGI_SWAP_CHAIN_DESC1 d1{d->BufferDesc.Width,d->BufferDesc.Height,d->BufferDesc.Format,FALSE,d->SampleDesc,
                d->BufferUsage,d->BufferCount,DXGI_SCALING_STRETCH,d->SwapEffect,DXGI_ALPHA_MODE_UNSPECIFIED,d->Flags};
            DXGI_SWAP_CHAIN_FULLSCREEN_DESC f{d->BufferDesc.RefreshRate,d->BufferDesc.ScanlineOrdering,d->BufferDesc.Scaling,d->Windowed};
            HRESULT hr=E_FAIL;
            if (HostCreate(q,d->OutputWindow,d1,&f,nullptr,_ReturnAddress(),out,hr)) return hr;
        }
#endif
        const HRESULT hr=original->CreateSwapChain(q,d,out);
        LogSwapchainCreation("CreateSwapChain",_ReturnAddress(),original.Get(),hr,out?*out:nullptr,creation.outer);
        if (SUCCEEDED(hr)&&out&&*out&&creation.outer) WrapSwapchain(out,q,this);
        return hr;
    }
    HRESULT STDMETHODCALLTYPE CreateSwapChainForHwnd(IUnknown* q,HWND w,const DXGI_SWAP_CHAIN_DESC1* d,
        const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* f,IDXGIOutput* o,IDXGISwapChain1** out) override {
        Lease lease(this); SwapchainCreation creation;
        auto* target=Revision<IDXGIFactory2>(2);
        if (!target) return E_NOINTERFACE;
#if MFG_UNLOCK_XESS
        if (creation.outer&&d&&out) {
            HRESULT hr=E_FAIL;
            if (HostCreate(q,w,*d,f,o,_ReturnAddress(),reinterpret_cast<IDXGISwapChain**>(out),hr)) return hr;
        }
#endif
        const HRESULT hr=target->CreateSwapChainForHwnd(q,w,d,f,o,out);
        LogSwapchainCreation("CreateSwapChainForHwnd",_ReturnAddress(),target,hr,out?*out:nullptr,creation.outer);
        if (SUCCEEDED(hr)&&out&&*out&&creation.outer) WrapSwapchain(reinterpret_cast<IDXGISwapChain**>(out),q,this);
        return hr;
    }
    HRESULT STDMETHODCALLTYPE CreateSwapChainForCoreWindow(IUnknown* q,IUnknown* w,const DXGI_SWAP_CHAIN_DESC1* d,
        IDXGIOutput* o,IDXGISwapChain1** out) override {
        Lease lease(this); SwapchainCreation creation;
        auto* target=Revision<IDXGIFactory2>(2);
        if (!target) return E_NOINTERFACE;
        const HRESULT hr=target->CreateSwapChainForCoreWindow(q,w,d,o,out);
        LogSwapchainCreation("CreateSwapChainForCoreWindow",_ReturnAddress(),target,hr,out?*out:nullptr,creation.outer);
        if (SUCCEEDED(hr)&&out&&*out&&creation.outer) WrapSwapchain(reinterpret_cast<IDXGISwapChain**>(out),q,this);
        return hr;
    }
    HRESULT STDMETHODCALLTYPE CreateSwapChainForComposition(IUnknown* q,const DXGI_SWAP_CHAIN_DESC1* d,
        IDXGIOutput* o,IDXGISwapChain1** out) override {
        Lease lease(this); SwapchainCreation creation;
        auto* target=Revision<IDXGIFactory2>(2);
        if (!target) return E_NOINTERFACE;
        const HRESULT hr=target->CreateSwapChainForComposition(q,d,o,out);
        LogSwapchainCreation("CreateSwapChainForComposition",_ReturnAddress(),target,hr,out?*out:nullptr,creation.outer);
        if (SUCCEEDED(hr)&&out&&*out&&creation.outer) WrapSwapchain(reinterpret_cast<IDXGISwapChain**>(out),q,this);
        return hr;
    }
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID Name, UINT DataSize, const void *pData) override { Lease lease(this); return interfaces.As<IDXGIFactory>(0)->SetPrivateData(Name,DataSize,pData); }
    HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID Name, const IUnknown *pUnknown) override { Lease lease(this); return interfaces.As<IDXGIFactory>(0)->SetPrivateDataInterface(Name,pUnknown); }
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID Name, UINT *pDataSize, void *pData) override { Lease lease(this); return interfaces.As<IDXGIFactory>(0)->GetPrivateData(Name,pDataSize,pData); }
    HRESULT STDMETHODCALLTYPE GetParent(REFIID riid, void **ppParent) override { Lease lease(this); return interfaces.As<IDXGIFactory>(0)->GetParent(riid,ppParent); }
    HRESULT STDMETHODCALLTYPE EnumAdapters(UINT Adapter, IDXGIAdapter **ppAdapter) override { Lease lease(this); return interfaces.As<IDXGIFactory>(0)->EnumAdapters(Adapter,ppAdapter); }
    HRESULT STDMETHODCALLTYPE MakeWindowAssociation(HWND WindowHandle, UINT Flags) override { Lease lease(this); return interfaces.As<IDXGIFactory>(0)->MakeWindowAssociation(WindowHandle,Flags); }
    HRESULT STDMETHODCALLTYPE GetWindowAssociation(HWND *pWindowHandle) override { Lease lease(this); return interfaces.As<IDXGIFactory>(0)->GetWindowAssociation(pWindowHandle); }
    HRESULT STDMETHODCALLTYPE CreateSoftwareAdapter(HMODULE Module, IDXGIAdapter **ppAdapter) override { Lease lease(this); return interfaces.As<IDXGIFactory>(0)->CreateSoftwareAdapter(Module,ppAdapter); }
    HRESULT STDMETHODCALLTYPE EnumAdapters1(UINT Adapter, IDXGIAdapter1 **ppAdapter) override { Lease lease(this); auto* target=Revision<IDXGIFactory1>(1); return target?target->EnumAdapters1(Adapter,ppAdapter):E_NOINTERFACE; }
    BOOL STDMETHODCALLTYPE IsCurrent() override { Lease lease(this); auto* target=Revision<IDXGIFactory1>(1); return target?target->IsCurrent():FALSE; }
    BOOL STDMETHODCALLTYPE IsWindowedStereoEnabled() override { Lease lease(this); auto* target=Revision<IDXGIFactory2>(2); return target?target->IsWindowedStereoEnabled():FALSE; }
    HRESULT STDMETHODCALLTYPE GetSharedResourceAdapterLuid(HANDLE hResource, LUID *pLuid) override { Lease lease(this); auto* target=Revision<IDXGIFactory2>(2); return target?target->GetSharedResourceAdapterLuid(hResource,pLuid):E_NOINTERFACE; }
    HRESULT STDMETHODCALLTYPE RegisterStereoStatusWindow(HWND WindowHandle, UINT wMsg, DWORD *pdwCookie) override { Lease lease(this); auto* target=Revision<IDXGIFactory2>(2); return target?target->RegisterStereoStatusWindow(WindowHandle,wMsg,pdwCookie):E_NOINTERFACE; }
    HRESULT STDMETHODCALLTYPE RegisterStereoStatusEvent(HANDLE hEvent, DWORD *pdwCookie) override { Lease lease(this); auto* target=Revision<IDXGIFactory2>(2); return target?target->RegisterStereoStatusEvent(hEvent,pdwCookie):E_NOINTERFACE; }
    void STDMETHODCALLTYPE UnregisterStereoStatus(DWORD dwCookie) override { Lease lease(this); auto* target=Revision<IDXGIFactory2>(2); if(target)target->UnregisterStereoStatus(dwCookie); }
    HRESULT STDMETHODCALLTYPE RegisterOcclusionStatusWindow(HWND WindowHandle, UINT wMsg, DWORD *pdwCookie) override { Lease lease(this); auto* target=Revision<IDXGIFactory2>(2); return target?target->RegisterOcclusionStatusWindow(WindowHandle,wMsg,pdwCookie):E_NOINTERFACE; }
    HRESULT STDMETHODCALLTYPE RegisterOcclusionStatusEvent(HANDLE hEvent, DWORD *pdwCookie) override { Lease lease(this); auto* target=Revision<IDXGIFactory2>(2); return target?target->RegisterOcclusionStatusEvent(hEvent,pdwCookie):E_NOINTERFACE; }
    void STDMETHODCALLTYPE UnregisterOcclusionStatus(DWORD dwCookie) override { Lease lease(this); auto* target=Revision<IDXGIFactory2>(2); if(target)target->UnregisterOcclusionStatus(dwCookie); }
    UINT STDMETHODCALLTYPE GetCreationFlags() override { Lease lease(this); auto* target=Revision<IDXGIFactory3>(3); return target?target->GetCreationFlags():0; }
    HRESULT STDMETHODCALLTYPE EnumAdapterByLuid(LUID AdapterLuid, REFIID riid, void **ppvAdapter) override { Lease lease(this); auto* target=Revision<IDXGIFactory4>(4); return target?target->EnumAdapterByLuid(AdapterLuid,riid,ppvAdapter):E_NOINTERFACE; }
    HRESULT STDMETHODCALLTYPE EnumWarpAdapter(REFIID riid, void **ppvAdapter) override { Lease lease(this); auto* target=Revision<IDXGIFactory4>(4); return target?target->EnumWarpAdapter(riid,ppvAdapter):E_NOINTERFACE; }
    HRESULT STDMETHODCALLTYPE CheckFeatureSupport(DXGI_FEATURE Feature, void *pFeatureSupportData, UINT FeatureSupportDataSize) override { Lease lease(this); auto* target=Revision<IDXGIFactory5>(5); return target?target->CheckFeatureSupport(Feature,pFeatureSupportData,FeatureSupportDataSize):E_NOINTERFACE; }
    HRESULT STDMETHODCALLTYPE EnumAdapterByGpuPreference(UINT Adapter, DXGI_GPU_PREFERENCE GpuPreference, REFIID riid, void **ppvAdapter) override { Lease lease(this); auto* target=Revision<IDXGIFactory6>(6); return target?target->EnumAdapterByGpuPreference(Adapter,GpuPreference,riid,ppvAdapter):E_NOINTERFACE; }
    HRESULT STDMETHODCALLTYPE RegisterAdaptersChangedEvent(HANDLE hEvent, DWORD *pdwCookie) override { Lease lease(this); auto* target=Revision<IDXGIFactory7>(7); return target?target->RegisterAdaptersChangedEvent(hEvent,pdwCookie):E_NOINTERFACE; }
    HRESULT STDMETHODCALLTYPE UnregisterAdaptersChangedEvent(DWORD dwCookie) override { Lease lease(this); auto* target=Revision<IDXGIFactory7>(7); return target?target->UnregisterAdaptersChangedEvent(dwCookie):E_NOINTERFACE; }
};

class SwapchainProxy;
struct PresentCall {
    SwapchainProxy* object;
    PresentCall* previous;
    bool outer=true;
    static thread_local PresentCall* current;
    explicit PresentCall(SwapchainProxy* p) : object(p),previous(current) {
        for (auto* call=previous;call;call=call->previous) if(call->object==p) outer=false;
        current=this;
    }
    ~PresentCall(){current=previous;}
};
thread_local PresentCall* PresentCall::current=nullptr;

class SwapchainProxy final : public IDXGISwapChain4 {
public:
    ComPtr<IDXGISwapChain> original;
    InterfaceCache<IDXGISwapChain,5> interfaces;
    ComPtr<IDXGIFactory> parent;
    dx12::Session* renderer;
    const unsigned version;
    std::atomic<ULONG> refs{1};
    // XeSS provider swapchains retire with their FG context; other chains
    // never set a provider generation and never retire early.
    std::mutex methodMutex;
    ComPtr<IDXGISwapChain> providerChain;
    uint64_t providerGeneration = 0;
    uint32_t activeMethods = 0;
    bool providerRetired = false;
    // xefgmenu.1: XeFG's native output chain under OptiScaler (generation =
    // its context's): the menu only, XeFG's sync intervals unchanged.
    bool outputLayer = false;
#if MFG_UNLOCK_XESS
    // xemfgsa.1: XeFG's proxy chain of the XeMFG standalone host (xess/d3d12_host.h).
    bool xemfgHost = false;
#endif
    struct Method {
        SwapchainProxy* object;
        bool valid;
        explicit Method(SwapchainProxy* p) : object(p) {
            object->AddRef();
            std::lock_guard lock(object->methodMutex);
            valid = !object->providerRetired;
            if (valid) ++object->activeMethods;
        }
        ~Method() {
            if (valid) { std::lock_guard lock(object->methodMutex); --object->activeMethods; }
            object->Release();
        }
    };
    bool RetireProvider(uint64_t generation) noexcept {
        {
            std::lock_guard lock(methodMutex);
            if (!generation || generation != providerGeneration || providerRetired || activeMethods) return false;
            // Never wait for a method which may be acquiring the XeSS context
            // lock held by this destroy caller. Reserve against every new call.
            providerRetired = true;
        }
        if (!dx12::RetireSession(renderer)) {
            std::lock_guard lock(methodMutex);
            providerRetired = false;
            return false;
        }
        dx12::DestroySession(renderer); renderer = nullptr;
        interfaces.Clear(); original.Reset(); parent.Reset(); providerChain.Reset();
        return true;
    }
    // xefgmenu.1: XeFG's output chain loses its menu before XeFG's destruction,
    // which releases this chain as its last user; until then it keeps
    // forwarding. New methods wait on the lock while the session retires.
    bool RetireRenderer(uint64_t generation) noexcept {
        std::lock_guard lock(methodMutex);
        if (!outputLayer || !generation || generation != providerGeneration || providerRetired || activeMethods) return false;
        if (!renderer) return true;
        if (!dx12::RetireSession(renderer)) return false;
        dx12::DestroySession(renderer); renderer = nullptr;
        return true;
    }
    SwapchainProxy(ComPtr<IDXGISwapChain>&& base,IDXGIFactory* factory,dx12::Session* session,unsigned v) noexcept
        : original(std::move(base)),interfaces(original.Get(),v),parent(factory),renderer(session),version(v) { ++chains; }
    ~SwapchainProxy() {
        dx12::DestroySession(renderer);
#if MFG_UNLOCK_XESS
        IDXGISwapChain* const host = xemfgHost ? original.Get() : nullptr;   // identity only after the Reset
#endif
        // No proxy/renderer locks are held while the existing chain executes
        // its final Release and any nested Steam/Streamline callbacks.
        interfaces.Clear(); original.Reset(); parent.Reset(); --liveChains;
#if MFG_UNLOCK_XESS
        if (host) xess_d3d12::ChainReleased(host);
#endif
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return refs.fetch_add(1,std::memory_order_relaxed)+1; }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG left=refs.fetch_sub(1,std::memory_order_acq_rel)-1;
        if (!left) delete this;
        return left;
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid,void** output) override {
        if (!output) return E_POINTER;
        *output = nullptr;
        Method lease(this);
        if (!lease.valid) {
            // The inert shell still has ordinary COM identity and can release
            // safely after SDK teardown; it never touches the retired provider.
            if (iid == __uuidof(IUnknown) || iid == chainId) {
                *output = static_cast<IDXGISwapChain4*>(this); AddRef(); return S_OK;
            }
            return E_NOINTERFACE;
        }
        const int v=Version(iid,chainIids);
        if (iid==__uuidof(IUnknown)||iid==__uuidof(IDXGIObject)||iid==__uuidof(IDXGIDeviceSubObject)
            ||iid==chainId||(v>=0&&interfaces.Acquire(unsigned(v),iid,chainLast[v]))) {
            *output=static_cast<IDXGISwapChain4*>(this); AddRef(); return S_OK;
        }
        return original->QueryInterface(iid,output);
    }
    HRESULT STDMETHODCALLTYPE GetParent(REFIID iid,void** output) override {
        Method lease(this); if (!lease.valid) return DXGI_ERROR_INVALID_CALL;
        return parent ? parent->QueryInterface(iid,output) : original->GetParent(iid,output);
    }
    // The optional V-Sync Off override applies to the application's own
    // top-level Presents only; nested calls keep their arguments.
    vsync_control::Submission AdjustSync(const PresentCall& call,UINT sync,UINT flags) noexcept {
        if (!call.outer || outputLayer) { vsync_control::Submission same{}; same.interval=same.originalInterval=sync; return same; }
        return vsync_control::BeforeApplicationPresent(sync,flags,dx12::IsPrincipal(renderer));
    }
    HRESULT STDMETHODCALLTYPE Present(UINT sync,UINT flags) override {
        Method lease(this); if (!lease.valid) return DXGI_ERROR_INVALID_CALL; PresentCall call(this);
#if MFG_UNLOCK_XESS
        if (call.outer && providerChain && !(flags & DXGI_PRESENT_TEST))
            xess_mod::PreparePresent(providerChain.Get(), providerGeneration);
#endif
        const bool entered=call.outer&&dx12::BeginPresent(renderer,flags,false);
        const auto vsync=AdjustSync(call,sync,flags);
#if MFG_UNLOCK_XESS
        const bool host=xemfgHost&&call.outer&&!(flags & DXGI_PRESENT_TEST);
        if (host) xess_d3d12::BeforePresent(original.Get());
#endif
        const HRESULT hr=original->Present(vsync.interval,flags);
#if MFG_UNLOCK_XESS
        if (host) xess_d3d12::AfterPresent(original.Get(),hr);
#endif
        vsync_control::AfterApplicationPresent(vsync,hr);
        if (entered) dx12::EndPresent(renderer,hr,flags);
        return hr;
    }
    HRESULT STDMETHODCALLTYPE Present1(UINT sync,UINT flags,const DXGI_PRESENT_PARAMETERS* params) override {
        Method lease(this); if (!lease.valid) return DXGI_ERROR_INVALID_CALL; PresentCall call(this);
#if MFG_UNLOCK_XESS
        if (call.outer && providerChain && !(flags & DXGI_PRESENT_TEST))
            xess_mod::PreparePresent(providerChain.Get(), providerGeneration);
#endif
        const bool partial=params&&(params->DirtyRectsCount||params->pScrollRect||params->pScrollOffset);
        const bool entered=call.outer&&dx12::BeginPresent(renderer,flags,partial);
        const auto vsync=AdjustSync(call,sync,flags);
#if MFG_UNLOCK_XESS
        const bool host=xemfgHost&&call.outer&&!(flags & DXGI_PRESENT_TEST);
        if (host) xess_d3d12::BeforePresent(original.Get());
#endif
        const HRESULT hr=interfaces.As<IDXGISwapChain1>(1)->Present1(vsync.interval,flags,params);
#if MFG_UNLOCK_XESS
        if (host) xess_d3d12::AfterPresent(original.Get(),hr);
#endif
        vsync_control::AfterApplicationPresent(vsync,hr);
        if (entered) dx12::EndPresent(renderer,hr,flags);
        return hr;
    }
    HRESULT STDMETHODCALLTYPE ResizeBuffers(UINT count,UINT w,UINT h,DXGI_FORMAT format,UINT flags) override {
        Method lease(this); if (!lease.valid) return DXGI_ERROR_INVALID_CALL;
        if (!dx12::BeginResize(renderer)) return DXGI_ERROR_WAS_STILL_DRAWING;
        const HRESULT hr=original->ResizeBuffers(count,w,h,format,flags);
        dx12::EndResize(renderer,hr); return hr;
    }
    HRESULT STDMETHODCALLTYPE ResizeBuffers1(UINT count,UINT w,UINT h,DXGI_FORMAT format,UINT flags,
        const UINT* masks,IUnknown* const* queues) override {
        Method lease(this); if (!lease.valid) return DXGI_ERROR_INVALID_CALL;
        if (!dx12::BeginResize(renderer)) return DXGI_ERROR_WAS_STILL_DRAWING;
        const HRESULT hr=interfaces.As<IDXGISwapChain3>(3)->ResizeBuffers1(count,w,h,format,flags,masks,queues);
        if (SUCCEEDED(hr)) dx12::VerifyResizeQueues(renderer,count,queues);
        dx12::EndResize(renderer,hr); return hr;
    }
    HRESULT STDMETHODCALLTYPE SetColorSpace1(DXGI_COLOR_SPACE_TYPE color) override {
        Method lease(this); if (!lease.valid) return DXGI_ERROR_INVALID_CALL;
        const HRESULT hr=interfaces.As<IDXGISwapChain3>(3)->SetColorSpace1(color);
        if (SUCCEEDED(hr)) dx12::ObserveColorSpace(renderer,color);
        return hr;
    }
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID Name, UINT DataSize, const void *pData) override { Method lease(this); if (!lease.valid) return DXGI_ERROR_INVALID_CALL; return interfaces.As<IDXGISwapChain>(0)->SetPrivateData(Name,DataSize,pData); }
    HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID Name, const IUnknown *pUnknown) override { Method lease(this); if (!lease.valid) return DXGI_ERROR_INVALID_CALL; return interfaces.As<IDXGISwapChain>(0)->SetPrivateDataInterface(Name,pUnknown); }
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID Name, UINT *pDataSize, void *pData) override { Method lease(this); if (!lease.valid) return DXGI_ERROR_INVALID_CALL; return interfaces.As<IDXGISwapChain>(0)->GetPrivateData(Name,pDataSize,pData); }
    HRESULT STDMETHODCALLTYPE GetDevice(REFIID riid, void **ppDevice) override { Method lease(this); if (!lease.valid) return DXGI_ERROR_INVALID_CALL; return interfaces.As<IDXGISwapChain>(0)->GetDevice(riid,ppDevice); }
    HRESULT STDMETHODCALLTYPE GetBuffer(UINT Buffer, REFIID riid, void **ppSurface) override { Method lease(this); if (!lease.valid) return DXGI_ERROR_INVALID_CALL; return interfaces.As<IDXGISwapChain>(0)->GetBuffer(Buffer,riid,ppSurface); }
    HRESULT STDMETHODCALLTYPE SetFullscreenState(BOOL Fullscreen, IDXGIOutput *pTarget) override { Method lease(this); if (!lease.valid) return DXGI_ERROR_INVALID_CALL; return interfaces.As<IDXGISwapChain>(0)->SetFullscreenState(Fullscreen,pTarget); }
    HRESULT STDMETHODCALLTYPE GetFullscreenState(BOOL *pFullscreen, IDXGIOutput **ppTarget) override { Method lease(this); if (!lease.valid) return DXGI_ERROR_INVALID_CALL; return interfaces.As<IDXGISwapChain>(0)->GetFullscreenState(pFullscreen,ppTarget); }
    HRESULT STDMETHODCALLTYPE GetDesc(DXGI_SWAP_CHAIN_DESC *pDesc) override { Method lease(this); if (!lease.valid) return DXGI_ERROR_INVALID_CALL; return interfaces.As<IDXGISwapChain>(0)->GetDesc(pDesc); }
    HRESULT STDMETHODCALLTYPE ResizeTarget(const DXGI_MODE_DESC *pNewTargetParameters) override { Method lease(this); if (!lease.valid) return DXGI_ERROR_INVALID_CALL; return interfaces.As<IDXGISwapChain>(0)->ResizeTarget(pNewTargetParameters); }
    HRESULT STDMETHODCALLTYPE GetContainingOutput(IDXGIOutput **ppOutput) override { Method lease(this); if (!lease.valid) return DXGI_ERROR_INVALID_CALL; return interfaces.As<IDXGISwapChain>(0)->GetContainingOutput(ppOutput); }
    HRESULT STDMETHODCALLTYPE GetFrameStatistics(DXGI_FRAME_STATISTICS *pStats) override { Method lease(this); if (!lease.valid) return DXGI_ERROR_INVALID_CALL; return interfaces.As<IDXGISwapChain>(0)->GetFrameStatistics(pStats); }
    HRESULT STDMETHODCALLTYPE GetLastPresentCount(UINT *pLastPresentCount) override { Method lease(this); if (!lease.valid) return DXGI_ERROR_INVALID_CALL; return interfaces.As<IDXGISwapChain>(0)->GetLastPresentCount(pLastPresentCount); }
    HRESULT STDMETHODCALLTYPE GetDesc1(DXGI_SWAP_CHAIN_DESC1 *pDesc) override { Method lease(this); if (!lease.valid) return DXGI_ERROR_INVALID_CALL; return interfaces.As<IDXGISwapChain1>(1)->GetDesc1(pDesc); }
    HRESULT STDMETHODCALLTYPE GetFullscreenDesc(DXGI_SWAP_CHAIN_FULLSCREEN_DESC *pDesc) override { Method lease(this); if (!lease.valid) return DXGI_ERROR_INVALID_CALL; return interfaces.As<IDXGISwapChain1>(1)->GetFullscreenDesc(pDesc); }
    HRESULT STDMETHODCALLTYPE GetHwnd(HWND *pHwnd) override { Method lease(this); if (!lease.valid) return DXGI_ERROR_INVALID_CALL; return interfaces.As<IDXGISwapChain1>(1)->GetHwnd(pHwnd); }
    HRESULT STDMETHODCALLTYPE GetCoreWindow(REFIID refiid, void **ppUnk) override { Method lease(this); if (!lease.valid) return DXGI_ERROR_INVALID_CALL; return interfaces.As<IDXGISwapChain1>(1)->GetCoreWindow(refiid,ppUnk); }
    BOOL STDMETHODCALLTYPE IsTemporaryMonoSupported() override { Method lease(this); if (!lease.valid) return 0; return interfaces.As<IDXGISwapChain1>(1)->IsTemporaryMonoSupported(); }
    HRESULT STDMETHODCALLTYPE GetRestrictToOutput(IDXGIOutput **ppRestrictToOutput) override { Method lease(this); if (!lease.valid) return DXGI_ERROR_INVALID_CALL; return interfaces.As<IDXGISwapChain1>(1)->GetRestrictToOutput(ppRestrictToOutput); }
    HRESULT STDMETHODCALLTYPE SetBackgroundColor(const DXGI_RGBA *pColor) override { Method lease(this); if (!lease.valid) return DXGI_ERROR_INVALID_CALL; return interfaces.As<IDXGISwapChain1>(1)->SetBackgroundColor(pColor); }
    HRESULT STDMETHODCALLTYPE GetBackgroundColor(DXGI_RGBA *pColor) override { Method lease(this); if (!lease.valid) return DXGI_ERROR_INVALID_CALL; return interfaces.As<IDXGISwapChain1>(1)->GetBackgroundColor(pColor); }
    HRESULT STDMETHODCALLTYPE SetRotation(DXGI_MODE_ROTATION Rotation) override { Method lease(this); if (!lease.valid) return DXGI_ERROR_INVALID_CALL; return interfaces.As<IDXGISwapChain1>(1)->SetRotation(Rotation); }
    HRESULT STDMETHODCALLTYPE GetRotation(DXGI_MODE_ROTATION *pRotation) override { Method lease(this); if (!lease.valid) return DXGI_ERROR_INVALID_CALL; return interfaces.As<IDXGISwapChain1>(1)->GetRotation(pRotation); }
    HRESULT STDMETHODCALLTYPE SetSourceSize(UINT Width, UINT Height) override { Method lease(this); if (!lease.valid) return DXGI_ERROR_INVALID_CALL; return interfaces.As<IDXGISwapChain2>(2)->SetSourceSize(Width,Height); }
    HRESULT STDMETHODCALLTYPE GetSourceSize(UINT *pWidth, UINT *pHeight) override { Method lease(this); if (!lease.valid) return DXGI_ERROR_INVALID_CALL; return interfaces.As<IDXGISwapChain2>(2)->GetSourceSize(pWidth,pHeight); }
    HRESULT STDMETHODCALLTYPE SetMaximumFrameLatency(UINT MaxLatency) override { Method lease(this); if (!lease.valid) return DXGI_ERROR_INVALID_CALL; return interfaces.As<IDXGISwapChain2>(2)->SetMaximumFrameLatency(MaxLatency); }
    HRESULT STDMETHODCALLTYPE GetMaximumFrameLatency(UINT *pMaxLatency) override { Method lease(this); if (!lease.valid) return DXGI_ERROR_INVALID_CALL; return interfaces.As<IDXGISwapChain2>(2)->GetMaximumFrameLatency(pMaxLatency); }
    HANDLE STDMETHODCALLTYPE GetFrameLatencyWaitableObject() override { Method lease(this); if (!lease.valid) return nullptr; return interfaces.As<IDXGISwapChain2>(2)->GetFrameLatencyWaitableObject(); }
    HRESULT STDMETHODCALLTYPE SetMatrixTransform(const DXGI_MATRIX_3X2_F *pMatrix) override { Method lease(this); if (!lease.valid) return DXGI_ERROR_INVALID_CALL; return interfaces.As<IDXGISwapChain2>(2)->SetMatrixTransform(pMatrix); }
    HRESULT STDMETHODCALLTYPE GetMatrixTransform(DXGI_MATRIX_3X2_F *pMatrix) override { Method lease(this); if (!lease.valid) return DXGI_ERROR_INVALID_CALL; return interfaces.As<IDXGISwapChain2>(2)->GetMatrixTransform(pMatrix); }
    UINT STDMETHODCALLTYPE GetCurrentBackBufferIndex() override { Method lease(this); if (!lease.valid) return 0; return interfaces.As<IDXGISwapChain3>(3)->GetCurrentBackBufferIndex(); }
    HRESULT STDMETHODCALLTYPE CheckColorSpaceSupport(DXGI_COLOR_SPACE_TYPE ColorSpace, UINT *pColorSpaceSupport) override { Method lease(this); if (!lease.valid) return DXGI_ERROR_INVALID_CALL; return interfaces.As<IDXGISwapChain3>(3)->CheckColorSpaceSupport(ColorSpace,pColorSpaceSupport); }
    HRESULT STDMETHODCALLTYPE SetHDRMetaData(DXGI_HDR_METADATA_TYPE Type, UINT Size, void *pMetaData) override {
        Method lease(this); if (!lease.valid) return DXGI_ERROR_INVALID_CALL;
        // GTA may call this method on the creation-return pointer before it
        // requests revision 4 from our proxy. Acquire that public interface on
        // first use, retaining the exact owning layer and normal cache lifetime.
        // Never assume a revision-3 pointer provides revision-4 methods.
        if (!interfaces.Acquire(4,chainIids[4],chainLast[4])) return E_NOINTERFACE;
        auto* target=interfaces.As<IDXGISwapChain4>(4);
        return target?target->SetHDRMetaData(Type,Size,pMetaData):E_NOINTERFACE;
    }
};

// alpha.15 (B4-26): a DirectX 11 (or 10) device where a D3D12 queue belongs (Metro Redux). Log only: the session
// below still refuses it as before.
bool OlderApiDevice(IUnknown* queue) noexcept {
    static constexpr GUID kD3D11Device{0xdb6f6ddb,0xac77,0x4e88,{0x82,0x53,0x81,0x9d,0xf9,0xbb,0xf1,0x40}};
    static constexpr GUID kD3D10Device{0x9b7e4c0f,0x342c,0x4106,{0xa1,0x9f,0x4f,0x27,0x04,0xf6,0x89,0xf0}};
    ComPtr<ID3D12CommandQueue> d3d12;
    if (SUCCEEDED(queue->QueryInterface(IID_PPV_ARGS(&d3d12)))) return false;
    for (const GUID& iid : {kD3D11Device,kD3D10Device}) {
        ComPtr<IUnknown> device;
        if (SUCCEEDED(queue->QueryInterface(iid,reinterpret_cast<void**>(device.GetAddressOf())))) return true;
    }
    return false;
}

void WrapSwapchain(IDXGISwapChain** output,IUnknown* queue,IDXGIFactory* parent) noexcept {
    if (!output||!*output||!queue||native::InsideLoader()||liveChains.load()>=64) return;
    static std::atomic<bool> olderApiLogged{false};
    if (!olderApiLogged.load(std::memory_order_relaxed) && OlderApiDevice(queue) && !olderApiLogged.exchange(true))
        single_module::Log(L"MFG_PROXY_UI DirectX 11 swap chain: the menu and XeMFG standalone need DirectX 12 or Vulkan; use OptiScaler for this game");
    Capacity capacity(liveChains); if(!capacity.held)return;
    InternalScope internal;
    if (!native::PinInterface(*output,2)||AlreadyWrapped(*output,chainId)) return;
    ComPtr<IDXGISwapChain3> chain3;
    if(FAILED((*output)->QueryInterface(IID_PPV_ARGS(&chain3))))return;
    ComPtr<IDXGISwapChain> base=chain3;
    constexpr unsigned version=3;
    if(!native::PinInterface(base.Get(),chainLast[version])||slots::Fault("proxy-chain")) return;
    if (!install::PrepareInput()) return;
    auto* session=dx12::CreateSession(base.Get(),queue);
    if (!session) return; // Unknown ownership leaves the original object untouched.
    auto* proxy=new(std::nothrow) SwapchainProxy(std::move(base),parent,session,version);
    if (!proxy) { dx12::DestroySession(session); return; }
    auto* old=*output;
    capacity.Transfer();
    *output=proxy;
    old->Release(); // Transfer only the reference returned by successful creation.
    single_module::Log(L"MFG_PROXY_UI swapchain proxy attached after existing creation chain completed; nativeTableWrites=0");
}
#if MFG_UNLOCK_XESS
// xemfgsa.1: a proxy over the XeMFG host's chain (XeFG's proxy chain) that carries the host's present calls and the
// game's release. No menu session: XeFG's chain is libxess_fg's object, which the renderer never takes; the menu sits
// on XeFG's output chain instead (AttachOutputSwapchain during the takeover).
bool WrapHostSwapchain(IDXGISwapChain** output,IUnknown*,IDXGIFactory* parent) noexcept {
    const auto refuse=[](const wchar_t* step) {
        wchar_t line[128]{}; swprintf_s(line,L"MFG_PROXY_UI XeMFG standalone chain not wrapped: %s",step);
        single_module::Log(line); return false;
    };
    if (!output||!*output) return false;
    if (native::InsideLoader()) return refuse(L"loader_lock");
    Capacity capacity(liveChains); if(!capacity.held) return refuse(L"capacity");
    InternalScope internal;
    // No AlreadyWrapped test: XeFG's proxy passes unknown interfaces to its output chain, which carries our menu
    // proxy (AttachOutputSwapchain), and this chain is the one the host just created.
    if (!native::PinInterface(*output,2)) return refuse(L"pin");
    ComPtr<IDXGISwapChain3> chain3;
    if (FAILED((*output)->QueryInterface(IID_PPV_ARGS(&chain3)))||!chain3) return refuse(L"swapchain3");
    ComPtr<IDXGISwapChain> base=chain3;
    if (!native::PinInterface(base.Get(),chainLast[3])) return refuse(L"pin_swapchain3");
    auto* proxy=new(std::nothrow) SwapchainProxy(std::move(base),parent,nullptr,3);
    if (!proxy) return refuse(L"allocation");
    proxy->xemfgHost=true;
    capacity.Transfer();
    auto* old=*output;
    *output=proxy;
    old->Release(); // The creation's reference moves to the proxy.
    single_module::Log(L"MFG_PROXY_UI XeMFG standalone chain attached");
    return true;
}
#endif

void WrapFactory(REFIID iid,void** output) noexcept {
    if (!output||!*output||!single_module::OverlayAllowed()||Version(iid,factoryIids)<0||liveFactories.load()>=64) return;
    Capacity capacity(liveFactories); if(!capacity.held)return;
    InternalScope internal;
    auto* returned=static_cast<IUnknown*>(*output);
    if (!native::PinInterface(returned,2)||AlreadyWrapped(returned,factoryId)) return;
    const unsigned version=static_cast<unsigned>(Version(iid,factoryIids));
    ComPtr<IDXGIFactory> base=static_cast<IDXGIFactory*>(returned);
    if(!native::PinInterface(base.Get(),factoryLast[version])||slots::Fault("proxy-factory")) return;
    auto* proxy=new(std::nothrow) FactoryProxy(std::move(base),version);
    if (!proxy) return;
    capacity.Transfer();
    *output=static_cast<IDXGIFactory7*>(proxy);
    returned->Release();
    gDxgiHooked.store(true);
    install::FactoryReady();
}
}
HRESULT FactoryCall(FactoryFn original,REFIID iid,void** output) noexcept {
    ++calls; Creation creation;
    const HRESULT hr=original(iid,output);
    if (SUCCEEDED(hr)&&creation.outer) WrapFactory(iid,output); else if (!creation.outer) ++skipped;
    return hr;
}
HRESULT FactoryCall(Factory2Fn original,UINT flags,REFIID iid,void** output) noexcept {
    ++calls; Creation creation;
    const HRESULT hr=original(flags,iid,output);
    if (SUCCEEDED(hr)&&creation.outer) WrapFactory(iid,output); else if (!creation.outer) ++skipped;
    return hr;
}
void ReadStatus(MfgSingleModuleStatus& status) noexcept {
    status.factoryGatewayCalls=calls.load();
    status.factoryWrappersCreated=factories.load();
    status.swapchainWrappersCreated=chains.load();
    status.liveFactoryWrappers=liveFactories.load();
    status.liveSwapchainWrappers=liveChains.load();
    status.internalFactoryCallsSkipped=skipped.load();
}
HRESULT ParentFactoryCall(ParentFn original,IDXGIObject* object,REFIID iid,void** output) noexcept {
    Creation creation;
    const HRESULT hr=original(object,iid,output);
    if (SUCCEEDED(hr)&&creation.outer) WrapFactory(iid,output);
    return hr;
}
bool Owned(IUnknown* object) noexcept {
    if (!object||!native::PinInterface(object,2)) return false;
    InternalScope internal;
    return AlreadyWrapped(object,factoryId)||AlreadyWrapped(object,chainId);
}
void Describe(IUnknown* object,wchar_t* text,size_t count) noexcept {
    if (!object) { swprintf_s(text,count,L"null"); return; }
    if (native::InsideLoader()) { swprintf_s(text,count,L"%p (loader lock)",static_cast<void*>(object)); return; }
    // The pin checks the table and its first entries before any call.
    if (!native::PinInterface(object,2)) { swprintf_s(text,count,L"%p unreadable-table",static_cast<void*>(object)); return; }
    InternalScope internal;
    const bool ours=AlreadyWrapped(object,factoryId)||AlreadyWrapped(object,chainId);
    struct Kind { IID iid; const char* name; };
    static const Kind kinds[]={{__uuidof(IDXGIFactory),"factory"},{__uuidof(IDXGISwapChain),"swapchain"},
        {__uuidof(ID3D12Device),"d3d12-device"},{__uuidof(ID3D12CommandQueue),"d3d12-queue"},{__uuidof(IDXGIAdapter),"adapter"}};
    const char* kind="other";
    for (const auto& candidate:kinds) {
        void* typed=nullptr;
        const HRESULT hr=object->QueryInterface(candidate.iid,&typed);
        if (typed) static_cast<IUnknown*>(typed)->Release();
        if (hr==S_OK&&typed) { kind=candidate.name; break; }
    }
    wchar_t table[160]{};
    ModuleAt((*reinterpret_cast<void***>(object))[0],table,160);
    swprintf_s(text,count,L"%p %hs ours=%u queryInterface=%s",static_cast<void*>(object),kind,static_cast<unsigned>(ours),table);
}
bool WrapUpgraded(void** output) noexcept {
    if (!output||!*output||native::InsideLoader()) return false;
    auto* upgraded=static_cast<IUnknown*>(*output);
    if (!native::PinInterface(upgraded,2)||Owned(upgraded)) return false;
    {
        // A D3D12 swapchain reports its device, never its presentation queue,
        // and the renderer needs that queue. Leave upgraded swapchains alone;
        // Streamline's manual-hooking flow upgrades the factory first.
        InternalScope internal;
        ComPtr<IDXGISwapChain> chain;
        if (SUCCEEDED(upgraded->QueryInterface(IID_PPV_ARGS(&chain)))&&chain) {
            static std::atomic_flag logged=ATOMIC_FLAG_INIT;
            if (!logged.test_and_set())
                single_module::Log(L"MFG_PROXY_UI upgraded swapchain left unwrapped: its presentation queue is not discoverable");
            return false;
        }
    }
    // The application keeps using its pointer at whatever revision it holds,
    // so the proxy's root is the highest revision the upgraded factory has.
    for (int version=7;version>=0;--version) {
        void* typed=nullptr;
        {
            InternalScope internal;
            if (upgraded->QueryInterface(factoryIids[version],&typed)!=S_OK||!typed) {
                if (typed) static_cast<IUnknown*>(typed)->Release();
                continue;
            }
        }
        void* wrapped=typed;
        WrapFactory(factoryIids[version],&wrapped);
        if (wrapped==typed) { static_cast<IUnknown*>(typed)->Release(); return false; }
        *output=wrapped;
        upgraded->Release(); // The application's reference moves to the proxy.
        return true;
    }
    return false;
}
}

namespace single_overlay::dx12 {
using Microsoft::WRL::ComPtr;
bool AttachProviderSwapchain(IDXGISwapChain* chain, ID3D12CommandQueue* queue,
    HMODULE provider, uint64_t generation, IDXGISwapChain** output) noexcept
{
    if (!output) return false;
    *output = nullptr;
    if (!chain || !queue || !provider || !generation || native::InsideLoader()) return false;
    proxy::Capacity capacity(proxy::liveChains);
    if (!capacity.held) return false;
    InternalScope internal;
    // Only the verified provider's own swapchain can take this route.
    if (!native::PinInterface(chain, 2)) return false;
    ComPtr<IDXGISwapChain3> version3;
    if (FAILED(chain->QueryInterface(IID_PPV_ARGS(&version3)))
        || !native::PinInterface(version3.Get(), 39)
        || !slots::ImageSlot(provider, *reinterpret_cast<void***>(version3.Get()))
        || !slots::ImageEntry(provider, (*reinterpret_cast<void***>(version3.Get()))[8])) return false;
    // OptiScaler mode keeps the provider wrapper for XeSS present preparation
    // (input fixes and UI composition) but creates no menu session or input hooks.
    const bool menu = single_module::OverlayAllowed();
    if (menu && !install::PrepareInput()) return false;
    auto* session = menu ? CreateSession(version3.Get(), queue, provider) : nullptr;
    if (menu && !session) return false;
    ComPtr<IDXGISwapChain> base = version3;
    auto* wrapper = new(std::nothrow) proxy::SwapchainProxy(std::move(base), nullptr, session, 3);
    if (!wrapper) { DestroySession(session); return false; }
    wrapper->providerGeneration = generation;
    wrapper->providerChain = chain;
    capacity.Transfer();
    *output = wrapper;
    single_module::Log(L"MFG_PROXY_UI XeSS provider swapchain attached with exact generation and queue");
    return true;
}
const char* AttachOutputSwapchain(IDXGISwapChain1** chain, IUnknown* queue, HWND window, uint64_t generation,
    IDXGISwapChain** keep) noexcept
{
    if (keep) *keep = nullptr;
    if (!chain || !*chain || !queue || !generation || !keep) return "arguments";
    if (!OutputSwapchainWanted()) return "ui_mode";
    if (native::InsideLoader()) return "loader_lock";
    // The factory route wraps what the game's creation returns (dlss5.8a/b):
    // one menu per window, never this chain as well. xemfgsa.1: XeMFG
    // standalone's takeover runs inside the factory route, and its game chain
    // gets no menu (WrapHostSwapchain), so the menu goes here.
#if MFG_UNLOCK_XESS
    if (proxy::swapchainCreations && !xess_d3d12::InsideTakeover()) return "factory_route";
#else
    if (proxy::swapchainCreations) return "factory_route";
#endif
    if (WindowSession(window)) return "window_session";
    proxy::Capacity capacity(proxy::liveChains);
    if (!capacity.held) return "capacity";
    InternalScope internal;
    ComPtr<IDXGISwapChain3> version3;
    if (!native::PinInterface(*chain, 2) || proxy::AlreadyWrapped(*chain, proxy::chainId)
        || FAILED((*chain)->QueryInterface(IID_PPV_ARGS(&version3)))
        || !native::PinInterface(version3.Get(), proxy::chainLast[3])) return "swapchain_interface";
    if (!install::PrepareInput()) return "input_unavailable";
    // Device, queue and LUID identity as for every session.
    auto* session = CreateSession(version3.Get(), queue, nullptr, true);
    if (!session) return "session_refused";
    ComPtr<IDXGISwapChain> base = version3;
    auto* wrapper = new(std::nothrow) proxy::SwapchainProxy(std::move(base), nullptr, session, 3);
    if (!wrapper) { DestroySession(session); return "allocation"; }
    wrapper->providerGeneration = generation;
    wrapper->outputLayer = true;
    capacity.Transfer();
    auto* created = *chain;
    *chain = wrapper; // XeFG's reference moves to the proxy.
    created->Release();
    wrapper->AddRef();
    *keep = wrapper;
    single_module::Log(L"MFG_PROXY_UI XeFG output swapchain attached (OptiScaler host route) with exact generation and queue");
    return nullptr;
}
bool RetireOutputSwapchain(IDXGISwapChain* chain, uint64_t generation) noexcept
{
    if (!chain || !generation) return false;
    ComPtr<IUnknown> owned;
    if (chain->QueryInterface(proxy::chainId, reinterpret_cast<void**>(owned.GetAddressOf())) != S_OK || !owned) return false;
    return static_cast<proxy::SwapchainProxy*>(owned.Get())->RetireRenderer(generation);
}
bool DetachProviderSwapchain(IDXGISwapChain* chain, uint64_t generation) noexcept
{
    if (!chain || !generation) return false;
    ComPtr<IUnknown> owned;
    if (chain->QueryInterface(proxy::chainId, reinterpret_cast<void**>(owned.GetAddressOf())) != S_OK || !owned) return false;
    return static_cast<proxy::SwapchainProxy*>(owned.Get())->RetireProvider(generation);
}
}
