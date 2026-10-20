// Actual provider discovery/entry installation; providers mapped as data, never
// initialized or executed. Heap/cache tests use caller-owned synthetic objects.
#include "dlss_sr_override.h"
#include <iostream>
#include <thread>
#include <vector>

unsigned checks=0,failures=0;
void Check(bool ok,const char* message){++checks;failures+=!ok;std::cout<<(ok?"PASS ":"FAIL ")<<message<<'\n';}
void ThrowSr(void*,uint32_t*,const char*,uint32_t,uint32_t,const char*)
{RaiseException(0xe0424242,0,0,nullptr);}
bool CatchSelectorSeh(dlss_sr_override::CachedValue* value,std::unique_lock<std::recursive_mutex>* lock)
{
    __try{dlss_sr_override::CallSr(&ThrowSr,nullptr,nullptr,nullptr,0,0,nullptr,value,lock);}
    __except(GetExceptionCode()==0xe0424242?EXCEPTION_EXECUTE_HANDLER:EXCEPTION_CONTINUE_SEARCH){return true;}
    return false;
}

int wmain(int argc,wchar_t** argv)
{
    if(argc!=3){std::cerr<<"usage: audit sr.dll rr.dll\n";return 2;}
    using namespace dlss_sr_override;
    for(unsigned k=0;k<2;++k)
    {
        const Kind kind=k==0?Kind::eSuperResolution:Kind::eRayReconstruction;
        HMODULE module=LoadLibraryExW(argv[k+1],nullptr,DONT_RESOLVE_DLL_REFERENCES);
        if(!module){std::cerr<<"provider mapping failed\n";return 2;}
        auto layout=Discover(module,kind);
        const auto base=reinterpret_cast<uintptr_t>(module);
        std::cout<<(k?"RR":"SR")<<" reader=0x"<<std::hex<<layout.reader.begin-base
            <<" selector=0x"<<layout.selector.begin-base<<" report=0x"<<layout.report.begin-base
            <<" effective=0x"<<layout.effective<<" candidate=0x"<<gDiscoveryAddress-base<<std::dec<<" stage="<<gDiscoveryStage<<'\n';
        Check(layout.module && layout.selector.begin && layout.report.begin && layout.reader.begin,
            "cached preset layout has three decoded, owned entries");
        if(!layout.selector.begin)return 1;
        Check(layout.effective==(k?0x1268:0x155c),"witnessed effective field discovered without an RVA/version list");
        Check(!Discover(module,k?Kind::eSuperResolution:Kind::eRayReconstruction).selector.begin,
            "SR and RR identities cannot substitute for each other");
        // A changed cache-load displacement must not be guessed from version
        // text. Mutate a private image page, verify refusal, restore exactly.
        uint8_t* witness=nullptr;
        for(auto a=layout.selector.begin;a+13<layout.selector.end;++a)
        {
            auto* p=reinterpret_cast<uint8_t*>(a);
            const char* shape=k?"\x48\x8b\x47\x08\x48\x8b\x08\x8b\x91":"\x49\x8b\x46\x08\x48\x8b\x08\x8b\x81";
            if(!memcmp(p,shape,9)){witness=p+9;break;}
        }
        if(!witness)return 2;
        const auto originalByte=*witness;DWORD protection=0,unused=0;
        if(!VirtualProtect(witness,1,PAGE_EXECUTE_WRITECOPY,&protection))return 2;
        *witness^=4;FlushInstructionCache(GetCurrentProcess(),witness,1);
        const bool refused=!Discover(module,kind).selector.begin;
        *witness=originalByte;FlushInstructionCache(GetCurrentProcess(),witness,1);
        if(!VirtualProtect(witness,1,protection,&unused))return 2;
        Check(refused&&Discover(module,kind).effective==layout.effective,
            "changed consumer layout fails closed; original byte/protection restore cleanly");
        Check(Ensure(module,kind)&&Supports(kind),"real registry installs all reader/report/selector detours");
        auto& owner=gSlots[k];
        alignas(16) std::array<uint8_t,0x1800> object{},second{};
        const uint32_t original=k?6:13;
        memcpy(object.data()+layout.effective,&original,4);
        memcpy(second.data()+layout.effective,&original,4);
        const auto saved=object;
        uintptr_t captured=reinterpret_cast<uintptr_t>(object.data());
        uintptr_t closure[2]{0,reinterpret_cast<uintptr_t>(&captured)};
        const auto caller=reinterpret_cast<void*>(layout.returns[0]);
        uint32_t selected=k?4:11;
        {
            Request request(kind,selected,true);
            std::lock_guard lock(owner.calls);
            CachedValue value(owner,closure,caller);
            Check(value.armed && !*reinterpret_cast<uint32_t*>(object.data()+layout.effective),
                "verified selector scope clears only the effective cached word");
            value.Observe(&selected,true);
            Check(request.Confirmed(),"a consumed selected preset establishes confirmation");
        }
        Check(object==saved,"provider cached state is restored byte-for-byte after the selector");
        captured=reinterpret_cast<uintptr_t>(second.data());
        {
            Request request(kind,selected,true);std::lock_guard lock(owner.calls);
            CachedValue value(owner,closure,caller);value.Observe(&selected,true);
            Check(value.armed && request.Confirmed(),"a new device object is resolved from the current call");
        }
        Check(second==saved && object==saved,"no stale device pointer survives another device/initialization");
        {
            Request request(kind,0,true);std::lock_guard lock(owner.calls);
            CachedValue value(owner,closure,caller);
            Check(!value.wanted && second==saved,"Game/driver preserves the cached NVIDIA app value");
        }
        {
            Request request(kind,selected,true);std::lock_guard lock(owner.calls);
            CachedValue value(owner,closure,caller);value.Observe(&selected,false);
            Check(!request.Confirmed(),"native selector/driver validation failure stays a failure");
        }
        Check(second==saved,"native failure still restores the cached override");
        {
            Request request(kind,selected,true);std::unique_lock lock(owner.calls);
            CachedValue value(owner,closure,caller);
            const bool caught=CatchSelectorSeh(&value,&lock);
            Check(caught&&!lock.owns_lock()&&second==saved,
                "native SEH unwind restores the cache and releases its gate");
        }
        {
            Request request(kind,selected,true);std::lock_guard lock(owner.calls);
            CachedValue value(owner,closure,reinterpret_cast<void*>(1));
            Check(!value.armed&&!request.Confirmed()&&second==saved,"unowned caller cannot mutate a cache");
        }
        std::atomic<bool> entered{false},release{false},observer{false};
        std::thread selector([&]{Request r(kind,selected,true);std::lock_guard lock(owner.calls);
            CachedValue v(owner,closure,caller);entered.store(true);entered.notify_one();release.wait(false);});
        entered.wait(false);
        std::thread report([&]{std::lock_guard lock(owner.calls);observer.store(second==saved);});
        release.store(true);release.notify_one();selector.join();report.join();
        Check(observer.load(),"another reader sees restored driver state after serialized consumption");
        Check(Ensure(module,kind),"repeated provider discovery retains the exact hook generation");
        {
            const auto deny=[]() noexcept {return false;};
            Request request(kind,selected,true,deny);std::lock_guard lock(owner.calls);
            CachedValue value(owner,closure,caller);
            Check(!value.armed&&!request.Confirmed()&&second==saved,"lost adapter eligibility prevents a cache write");
        }
        {
            Request request(kind,selected,true);std::lock_guard lock(owner.calls);
            CachedValue value(owner,closure,caller);
            const uint32_t foreign=7;memcpy(second.data()+layout.effective,&foreign,4);
            Check(!value.Restore()&&!Supports(kind)
                &&*reinterpret_cast<uint32_t*>(second.data()+layout.effective)==foreign,
                "restore CAS detects a conflicting writer, preserves it and disables the route");
        }
    }
    std::cout<<"SR_OVERRIDE_AUDIT checks="<<checks<<" failures="<<failures<<'\n';
    return failures?1:0;
}
