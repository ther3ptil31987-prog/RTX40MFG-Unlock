#pragma once

// SR/RR providers cache DRS presets per device. Suppress only the effective
// cached value while a proven Create-time preset selector consumes our hints,
// then restore it. No device/heap pointer survives that native call; Game/driver
// retains the provider's original cache and its native validation (including RR F).
#include "dlss_sr_policy.h"
#include "dlssg_preset_config.h"
#include "entry_detour.h"
#include <array>
#include <atomic>
#include <mutex>
#include <intrin.h>

namespace dlss_sr_override
{
using dlss_sr::Kind;
namespace image = dlssg_preset::config;
struct Layout
{
    HMODULE module = nullptr;
    Kind kind{};
    image::Function reader{}, report{}, selector{}, parent{};
    uint32_t effective = 0;
    std::array<uintptr_t, 6> returns{};
};
inline thread_local unsigned gDiscoveryStage=0;
inline thread_local uintptr_t gDiscoveryAddress=0;
inline image::Function LeafGap(HMODULE module,uintptr_t address) noexcept
{
    const auto base=reinterpret_cast<uintptr_t>(module);
    DWORD64 owner=0;
    if(RtlLookupFunctionEntry(address,&owner,nullptr))return {};
    const auto* nt=reinterpret_cast<const IMAGE_NT_HEADERS64*>(base
        +reinterpret_cast<const IMAGE_DOS_HEADER*>(base)->e_lfanew);
    const auto& dir=nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION];
    if(!dir.VirtualAddress || dir.Size%sizeof(RUNTIME_FUNCTION)
        || !image::Region(module,base+dir.VirtualAddress,dir.Size,false))return {};
    const auto* functions=reinterpret_cast<const RUNTIME_FUNCTION*>(base+dir.VirtualAddress);
    const auto rva=address-base;
    uint32_t lower=0,upper=0;
    for(size_t i=0;i<dir.Size/sizeof(RUNTIME_FUNCTION);++i)
    {
        if(functions[i].EndAddress<=rva)lower=std::max<uint32_t>(lower,functions[i].EndAddress);
        if(functions[i].BeginAddress>rva){upper=functions[i].BeginAddress;break;}
    }
    if(!lower||upper<=lower||upper-lower>0x10000||rva<lower||rva>=upper
        ||!image::Region(module,base+lower,upper-lower,true))return {};
    return {base+lower,base+upper};
}

inline Layout Discover(HMODULE module, Kind kind) noexcept
{
    __try
    {
        gDiscoveryStage=1;
        Layout result{}; result.module = module; result.kind = kind;
        const uint32_t key = kind == Kind::eSuperResolution ? dlss_sr::kDrsPresetSetting : dlss_sr::kDrsRrPresetSetting;
        // Find both app/global queries of this feature's own DRS key, in one
        // unwind-owned ReadValuesFromDRS function. No filename/version/RVA gate.
        unsigned queries = 0;
        if (!image::Sections(module, true, [&](const uint8_t* data, size_t length) {
            for (size_t i=0; i+10<=length; ++i)
            {
                uint32_t id=0; memcpy(&id, data+i+1, 4);
                if (data[i]!=0xb9 || id!=key) continue;
                auto f=image::Enclosing(module,reinterpret_cast<uintptr_t>(data+i));
                if (!f.begin || (result.reader.begin && result.reader.begin!=f.begin)) return false;
                result.reader=f; ++queries;
            }
            return true;
        })) return {};
        gDiscoveryStage=2;
        if (queries!=2) return {};
        // Each selector has a stable, typed integer/pointer ABI and a unique
        // cache load/test after dereferencing its closure's captured device.
        unsigned selectors=0;
        if (!image::Sections(module, true, [&](const uint8_t* data, size_t length) {
            for (size_t i=0; i+21<=length; ++i)
            {
                const auto* p=data+i;
                const bool sr=kind==Kind::eSuperResolution;
                if (sr ? memcmp(p,"\x49\x8b\x46\x08\x48\x8b\x08\x8b\x81",9)
                       : memcmp(p,"\x48\x8b\x47\x08\x48\x8b\x08\x8b\x91",9)) continue;
                uint32_t field=0; memcpy(&field,p+9,4);
                if (field<0x100 || field>0x10000 || (field&3)
                    || memcmp(p+13,sr?"\x85\xc0\x0f\x84":"\x85\xd2\x74",sr?4:3)) continue;
                if (++selectors!=1) return false;
                result.selector=image::Enclosing(module,reinterpret_cast<uintptr_t>(p));
                result.effective=field;
            }
            return true;
        })) return {};
        gDiscoveryStage=3;
        if (selectors!=1 || !result.selector.begin) return {};
        const bool sr=kind==Kind::eSuperResolution;
        constexpr uint8_t srEntry[]{0x40,0x53,0x55,0x56,0x57,0x41,0x56,0x41,0x57,0x48,0x81,0xec,0x98,0,0,0};
        constexpr uint8_t rrEntry[]{0x48,0x89,0x5c,0x24,0x08,0x48,0x89,0x6c,0x24,0x10,0x48,0x89,0x74,0x24,0x18,0x57,0x48,0x83,0xec,0x40};
        if (memcmp(reinterpret_cast<void*>(result.selector.begin),sr?srEntry:rrEntry,sr?sizeof(srEntry):sizeof(rrEntry))) return {};
        // Bind effective-field stores to the function that read the DRS key.
        // These audited functions put jump-table data after their final RET,
        // inside the unwind range: decode through that RET only.
        gDiscoveryStage=4;
        unsigned stores=0;
        if (!image::Walk(result.reader,[&](const uint8_t* p,const hde64s& in) {
            if ((in.flags&F_DISP32) && in.disp.disp32==result.effective && in.modrm_mod==2
                && in.modrm_rm==3 && (in.opcode==0x89 || in.opcode==0xc7) && !in.rex_w) ++stores;
            return true;
        },true) || stores!=(sr?4u:2u)) return {};
        // Six quality-mode calls, all from the same FillCreationParams parent;
        // exact return sites bind writes to native caller-owned output storage.
        gDiscoveryStage=5;
        unsigned calls=0;
        if (!image::Sections(module, true, [&](const uint8_t* data, size_t length) {
            for(size_t i=0;i+5<=length;++i)
            {
                const auto* p=data+i;
                if (p[0]!=0xe8 || image::Relative(p,1,5)!=result.selector.begin) continue;
                auto f=image::Enclosing(module,reinterpret_cast<uintptr_t>(p));
                if (!f.begin || calls==6 || (result.parent.begin && result.parent.begin!=f.begin)) return false;
                result.parent=f; result.returns[calls++]=reinterpret_cast<uintptr_t>(p+5);
            }
            return true;
        }) || calls!=6) return {};
        // Every scalar 32-bit reader of this effective field must be either the
        // selector or the provider's diagnostic FillCreationParams. Serialize
        // both plus ReadValuesFromDRS; a new reader makes the shape unsupported.
        gDiscoveryStage=6;
        unsigned reads=0;
        if (!image::Sections(module, true, [&](const uint8_t* data, size_t length) {
            for(size_t i=0;i+15<=length;++i)
            {
                // The displacement prefilter keeps decoding bounded on large providers.
                bool candidate=false;
                for(unsigned d=1;d<=4;++d) {uint32_t value=0;memcpy(&value,data+i+d,4);candidate|=value==result.effective;}
                if(!candidate) continue;
                hde64s in{};hde64_disasm(data+i,&in);
                if((in.flags&F_ERROR) || !(in.flags&F_DISP32) || in.disp.disp32!=result.effective
                    || in.modrm_mod!=2 || !((in.opcode==0x8b&&!in.rex_w)||in.opcode==0x63)) continue;
                const auto address=reinterpret_cast<uintptr_t>(data+i);
                gDiscoveryAddress=address;
                auto f=image::Enclosing(module,address);
                const bool leaf=!f.begin;
                if(leaf)f=LeafGap(module,address);
                bool boundary=false,interior=false;
                const auto classify=[&](const uint8_t* p,const hde64s& decoded){
                    const auto a=reinterpret_cast<uintptr_t>(p);
                    boundary|=a==address;interior|=address>a&&address<a+decoded.len;return true;};
                if(!image::Walk(f,classify,!leaf)) {gDiscoveryStage=62;return false;}
                if(!boundary){if(interior)continue;gDiscoveryStage=63;return false;}
                ++reads;
                if(f.begin==result.selector.begin) continue;
                if(result.report.begin && result.report.begin!=f.begin) {gDiscoveryStage=64;return false;}
                result.report=f;
            }
            return true;
        })) return {};
        gDiscoveryStage=7;
        if(reads!=(sr?4u:2u) || !result.report.begin) return {};
        // Diagnostic ABI: (NGXDLAA/NGXDLSSD*, uint32_t), no FP arguments.
        constexpr uint8_t reportEntry[]{0x48,0x89,0x5c,0x24,0x08,0x55,0x56,0x57,0x41,0x54,0x41,0x55,0x41,0x56,0x41,0x57};
        constexpr uint8_t rrReportEntry[]{0x48,0x89,0x5c,0x24,0x08,0x48,0x89,0x74,0x24,0x10,
            0x55,0x57,0x41,0x54,0x41,0x56,0x41,0x57};
        if(memcmp(reinterpret_cast<void*>(result.report.begin),sr?reportEntry:rrReportEntry,
            sr?sizeof(reportEntry):sizeof(rrReportEntry))) return {};
        gDiscoveryStage=8;
        return result;
    }
    __except(EXCEPTION_EXECUTE_HANDLER){return {};}
}

struct RequestState {Kind kind{};uint32_t preset=0;uint32_t reads=0;bool failed=false;bool(*eligible)() noexcept=nullptr;};
inline thread_local RequestState* gRequest=nullptr;
struct Request
{
    RequestState state{}; RequestState* previous=nullptr;
    Request(Kind kind,uint32_t preset,bool enabled,bool(*eligible)() noexcept=nullptr) noexcept
    {state.kind=kind;state.preset=preset;state.eligible=eligible;previous=gRequest;if(enabled)gRequest=&state;}
    ~Request(){gRequest=previous;}
    void Leave() noexcept {gRequest=previous;}
    bool Confirmed() const noexcept{return state.reads>0&&!state.failed;}
};
struct Slot
{
    std::atomic<HMODULE> module{nullptr}; Layout layout{};
    std::recursive_mutex calls;
    std::atomic<bool> ready{false},denied{false};
};
inline std::array<Slot,16> gSlots{};
inline std::mutex gInstall;
inline bool Current(const Slot& slot) noexcept
{
    if(!slot.ready.load(std::memory_order_acquire)||slot.denied.load(std::memory_order_acquire)) return false;
    const auto m=slot.module.load(std::memory_order_acquire);
    for(auto kind:{entry_detour::Kind::eSrPresetSelector,entry_detour::Kind::eSrPresetReader,entry_detour::Kind::eSrPresetReport})
    {
        const auto s=entry_detour::ReadSnapshot(kind,m);
        const auto target=kind==entry_detour::Kind::eSrPresetSelector?slot.layout.selector.begin
            :kind==entry_detour::Kind::eSrPresetReader?slot.layout.reader.begin:slot.layout.report.begin;
        if(!s.current||s.generation!=1||reinterpret_cast<uintptr_t>(s.target)!=target) return false;
    }
    return true;
}
inline bool Supports(Kind kind) noexcept
{
    bool found=false;
    for(const auto& s:gSlots) if(s.module.load(std::memory_order_acquire)&&s.layout.kind==kind)
    {found=true;if(!Current(s))return false;}
    return found;
}
inline bool PrivateWritable(uintptr_t address,size_t bytes) noexcept
{
    MEMORY_BASIC_INFORMATION m{};
    if(!address||!VirtualQuery(reinterpret_cast<void*>(address),&m,sizeof(m))||m.Type!=MEM_PRIVATE
        ||m.State!=MEM_COMMIT||(m.Protect&(PAGE_GUARD|PAGE_NOACCESS))||(m.Protect&0xff)!=PAGE_READWRITE)return false;
    const auto base=reinterpret_cast<uintptr_t>(m.BaseAddress);
    return address>=base && address-base<m.RegionSize && bytes<=m.RegionSize-(address-base);
}
inline bool Compare(volatile long* slot,long expected,long value) noexcept
{
    __try{return InterlockedCompareExchange(slot,value,expected)==expected && *slot==value;}
    __except(EXCEPTION_EXECUTE_HANDLER){return false;}
}
struct CachedValue
{
    Slot& owner; volatile long* word=nullptr;long before=0;bool armed=false;bool wanted=false;
    CachedValue(Slot& slot,void* closure,const void* caller):owner(slot)
    {
        wanted=gRequest && gRequest->kind==slot.layout.kind && dlss_sr::IsOverrideFor(gRequest->kind,gRequest->preset);
        if(!wanted)return;
        bool callerValid=false;for(auto address:slot.layout.returns)callerValid|=address==reinterpret_cast<uintptr_t>(caller);
        uintptr_t captured=0,object=0;
        const auto a=reinterpret_cast<uintptr_t>(closure);
        if((gRequest->eligible&&!gRequest->eligible())||!Current(slot)||!callerValid||!PrivateWritable(a,16)
            ||!provider_image::SafeRead(a+8,captured)||!PrivateWritable(captured,sizeof(uintptr_t))
            ||!provider_image::SafeRead(captured,object)||!PrivateWritable(object,slot.layout.effective+4)
            ||(object&7)||!provider_image::SafeRead(object+slot.layout.effective,before)
            ||before<0||before>15){gRequest->failed=true;return;}
        word=reinterpret_cast<volatile long*>(object+slot.layout.effective);
        armed=Compare(word,before,0);
        if(!armed)gRequest->failed=true;
    }
    bool Restore() noexcept
    {
        if(!armed)return !wanted;
        armed=false;
        if(Compare(word,0,before))return true;
        owner.denied.store(true,std::memory_order_release);
        if(gRequest)gRequest->failed=true;
        return false;
    }
    void Observe(uint32_t* value,bool succeeded) noexcept
    {
        if(!wanted)return;
        uint32_t seen=0;
        if(!armed||!succeeded||!PrivateWritable(reinterpret_cast<uintptr_t>(value),4)
            ||!provider_image::SafeCopy(&seen,value,4)||seen!=gRequest->preset)gRequest->failed=true;
        else ++gRequest->reads;
    }
    ~CachedValue(){Restore();}
};
using SrFn=void(*)(void*,uint32_t*,const char*,uint32_t,uint32_t,const char*);
using RrFn=uint32_t(*)(void*,uint32_t*,const char*,uint32_t,const char*);
using ReaderFn=void(*)(void*);
using ReportFn=uintptr_t(*)(void*,uint32_t);
// Provider failures can unwind through an NGX caller's SEH handler. Restore the
// cache and release the gate on those paths too, without relying on /EHa.
inline void CallSr(SrFn fn,void* a,uint32_t* b,const char* c,uint32_t d,uint32_t e,const char* f,
    CachedValue* value,std::unique_lock<std::recursive_mutex>* lock)
{
    __try {fn(a,b,c,d,e,f);value->Observe(b,true);}
    __finally {value->Restore();lock->unlock();}
}
inline uint32_t CallRr(RrFn fn,void* a,uint32_t* b,const char* c,uint32_t d,const char* e,
    CachedValue* value,std::unique_lock<std::recursive_mutex>* lock)
{
    __try {const auto r=fn(a,b,c,d,e);value->Observe(b,r==1);return r;}
    __finally {value->Restore();lock->unlock();}
}
inline void CallReader(ReaderFn fn,void* a,std::unique_lock<std::recursive_mutex>* lock)
{
    __try {fn(a);}
    __finally {lock->unlock();}
}
inline uintptr_t CallReport(ReportFn fn,void* a,uint32_t b,std::unique_lock<std::recursive_mutex>* lock)
{
    __try {return fn(a,b);}
    __finally {lock->unlock();}
}
template<size_t I> void SelectSr(void* a,uint32_t* b,const char* c,uint32_t d,uint32_t e,const char* f)
{
    const void* caller=_ReturnAddress();auto& s=gSlots[I];std::unique_lock lock(s.calls);
    auto original=reinterpret_cast<SrFn>(entry_detour::ReadSnapshot(entry_detour::Kind::eSrPresetSelector,s.module.load()).original);
    if(!original){if(gRequest)gRequest->failed=true;return;}
    CachedValue value(s,a,caller);CallSr(original,a,b,c,d,e,f,&value,&lock);
}
template<size_t I> uint32_t SelectRr(void* a,uint32_t* b,const char* c,uint32_t d,const char* e)
{
    const void* caller=_ReturnAddress();auto& s=gSlots[I];std::unique_lock lock(s.calls);
    auto original=reinterpret_cast<RrFn>(entry_detour::ReadSnapshot(entry_detour::Kind::eSrPresetSelector,s.module.load()).original);
    if(!original){if(gRequest)gRequest->failed=true;return 0xbad00001;}
    CachedValue value(s,a,caller);return CallRr(original,a,b,c,d,e,&value,&lock);
}
template<size_t I> void Read(void* a)
{
    auto& s=gSlots[I];std::unique_lock lock(s.calls);
    auto original=reinterpret_cast<ReaderFn>(entry_detour::ReadSnapshot(entry_detour::Kind::eSrPresetReader,s.module.load()).original);
    if(original)CallReader(original,a,&lock);
}
template<size_t I> uintptr_t Report(void* a,uint32_t b)
{
    auto& s=gSlots[I];std::unique_lock lock(s.calls);
    auto original=reinterpret_cast<ReportFn>(entry_detour::ReadSnapshot(entry_detour::Kind::eSrPresetReport,s.module.load()).original);
    return original?CallReport(original,a,b,&lock):0;
}
template<size_t...I> constexpr auto SrThunks(std::index_sequence<I...>){return std::array<SrFn,sizeof...(I)>{&SelectSr<I>...};}
template<size_t...I> constexpr auto RrThunks(std::index_sequence<I...>){return std::array<RrFn,sizeof...(I)>{&SelectRr<I>...};}
template<size_t...I> constexpr auto ReadThunks(std::index_sequence<I...>){return std::array<ReaderFn,sizeof...(I)>{&Read<I>...};}
template<size_t...I> constexpr auto ReportThunks(std::index_sequence<I...>){return std::array<ReportFn,sizeof...(I)>{&Report<I>...};}
inline constexpr auto srThunks=SrThunks(std::make_index_sequence<16>{});
inline constexpr auto rrThunks=RrThunks(std::make_index_sequence<16>{});
inline constexpr auto readThunks=ReadThunks(std::make_index_sequence<16>{});
inline constexpr auto reportThunks=ReportThunks(std::make_index_sequence<16>{});
inline bool Ensure(HMODULE module,Kind kind) noexcept
{
    for(const auto& s:gSlots)if(s.module.load(std::memory_order_acquire)==module)return s.layout.kind==kind&&Current(s);
    std::lock_guard lock(gInstall);
    for(size_t i=0;i<gSlots.size();++i)
    {
        auto& s=gSlots[i];if(s.module.load()==module)return s.layout.kind==kind&&Current(s);
        if(s.module.load())continue;
        HMODULE pin=nullptr;
        if(!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_PIN,
            reinterpret_cast<LPCWSTR>(module),&pin)||pin!=module)return false;
        s.layout=Discover(module,kind);s.layout.kind=kind;s.layout.module=module;
        s.module.store(module,std::memory_order_release);
        if(!s.layout.selector.begin)return false;
        entry_detour::InstallOptions options{};options.generation=1;options.allowRelocated=true;
        void* original=nullptr;
        const bool reader=entry_detour::Install(entry_detour::Kind::eSrPresetReader,module,reinterpret_cast<void*>(s.layout.reader.begin),reinterpret_cast<void*>(readThunks[i]),original,options);
        const bool report=entry_detour::Install(entry_detour::Kind::eSrPresetReport,module,reinterpret_cast<void*>(s.layout.report.begin),reinterpret_cast<void*>(reportThunks[i]),original,options);
        const bool selector=entry_detour::Install(entry_detour::Kind::eSrPresetSelector,module,reinterpret_cast<void*>(s.layout.selector.begin),
            kind==Kind::eSuperResolution?reinterpret_cast<void*>(srThunks[i]):reinterpret_cast<void*>(rrThunks[i]),original,options);
        s.ready.store(reader&&report&&selector,std::memory_order_release);return Current(s);
    }
    return false;
}
}
