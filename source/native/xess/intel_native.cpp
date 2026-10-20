#include "intel_native.h"
#include "runtime_layout.h"
#include "host_unlock.h"
#include "intel_native_profile.h"
#include "../provider_image.h"
#include <xefg_swapchain_d3d12.h>
#include <array>
#include <filesystem>
#include <intrin.h>
#include <mutex>
#include <utility>

namespace intel_native
{
namespace
{
using xess_layout::Image;
void Require(bool ok, const char* why) { if (!ok) throw std::runtime_error(why); }
uint32_t Resolve(const Image& image,uint32_t rva)
{
    for (unsigned depth=0;depth<4;++depth)
    {
        uint8_t opcode{};
        Require(image.Range(rva,5,1) && image.Read(rva,&opcode,1),"native_target_owner");
        if (opcode!=0xe9) return rva;
        rva=image.Target(rva,{1,5,-1,1,0});
    }
    throw std::runtime_error("native_target_depth");
}
bool Owned(HMODULE module,uintptr_t address,size_t bytes,DWORD protection)
{
    MEMORY_BASIC_INFORMATION m{};
    return address && bytes && VirtualQuery(reinterpret_cast<void*>(address),&m,sizeof(m))==sizeof(m)
        && m.AllocationBase==module && m.Type==MEM_IMAGE && m.State==MEM_COMMIT && m.Protect==protection
        && address>=uintptr_t(m.BaseAddress) && address-uintptr_t(m.BaseAddress)<m.RegionSize
        && bytes<=m.RegionSize-(address-uintptr_t(m.BaseAddress));
}
template<class T> T Read(uintptr_t address)
{
    T value{};
    Require(provider_image::SafeRead(address,value),"native_context_read"); return value;
}
HMODULE Owner(uintptr_t address)
{
    HMODULE module{};
    Require(GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCWSTR>(address),&module)!=FALSE,"native_callable_owner");
    Require(Owned(module,address,1,PAGE_EXECUTE_READ),"native_callable_protection"); return module;
}
uintptr_t Export(HMODULE module,const char* name)
{
    const auto address=uintptr_t(GetProcAddress(module,name));
    Require(address && Owner(address)==module,"native_export_owner"); return address;
}
std::filesystem::path Path(HMODULE module)
{
    std::array<wchar_t,32768> path{};
    const auto count=GetModuleFileNameW(module,path.data(),DWORD(path.size()));
    Require(count && count<path.size(),"native_module_path"); return path.data();
}
void Pin(HMODULE module)
{
    HMODULE pinned{};
    Require(GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
        reinterpret_cast<LPCWSTR>(module),&pinned) && pinned==module,"native_module_pin");
}
struct ModuleReference
{
    HMODULE module{};
    ~ModuleReference() { if (module) FreeLibrary(module); }
    HMODULE Release() noexcept { return std::exchange(module,nullptr); }
};
bool SameFile(const std::filesystem::path& first,const std::filesystem::path& second)
{
    // Accept aliases of the same physical sibling, never just a basename.
    const auto identity=[](const std::filesystem::path& path) {
        const auto file=CreateFileW(path.c_str(),FILE_READ_ATTRIBUTES,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
        Require(file!=INVALID_HANDLE_VALUE,"native_xell_identity_open");
        BY_HANDLE_FILE_INFORMATION info{};
        const bool ok=GetFileInformationByHandle(file,&info)!=FALSE;
        CloseHandle(file);
        Require(ok,"native_xell_identity_read");
        return info;
    };
    const auto a=identity(first),b=identity(second);
    return a.dwVolumeSerialNumber==b.dwVolumeSerialNumber && a.nFileIndexHigh==b.nFileIndexHigh
        && a.nFileIndexLow==b.nFileIndexLow && !(a.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY);
}
struct XellVersion { uint16_t major,minor,patch,reserved; };
bool Version(HMODULE module,XellVersion& version)
{
    const auto get=reinterpret_cast<int(__cdecl*)(XellVersion*)>(GetProcAddress(module,"xellGetVersion"));
    return get && get(&version)==0;
}
// The driver's own test before it looks for igxell: libxell above 1.0.0.
bool Handoff(const XellVersion& version)
{
    return version.major && (version.major!=1 || version.minor || version.patch);
}
// The same lookups the driver makes: loaded modules by base name, one
// reference each, nothing loaded.
struct DriverChoice
{
    ModuleReference libxell,igxell;
    XellVersion version{};
    bool versionKnown{};
    const char* Binding() const noexcept
    {
        if (!libxell.module) return "none";
        if (!versionKnown) return "unknown";
        return Handoff(version) && igxell.module ? "igxell" : "libxell";
    }
};
void Choose(DriverChoice& choice)
{
    GetModuleHandleExW(0,L"libxell.dll",&choice.libxell.module);
    GetModuleHandleExW(0,L"igxell.dll",&choice.igxell.module);
    choice.versionKnown=choice.libxell.module && Version(choice.libxell.module,choice.version);
}
struct Binding
{
    HMODULE fg{};
    xefg_swapchain_handle_t nested{};
    uintptr_t core{};
    CapacitySite site;
};
Binding Bind(HMODULE shim,void* handle)
{
    Require(shim && handle,"native_context_missing");
    Require(Read<uint32_t>(uintptr_t(handle))==0x47466558,"native_handle_magic");
    const auto implementation=Read<uintptr_t>(uintptr_t(handle)+8), table=Read<uintptr_t>(implementation);
    Require(Owned(shim,table,0x60,PAGE_READONLY),"native_forwarding_table");
    Image shimImage(shim);
    const auto entry=Read<uintptr_t>(table+8);
    Require(Owner(entry)==shim,"native_forwarding_owner");
    const auto method=Resolve(shimImage,uint32_t(entry-uintptr_t(shim)));
    std::array<uint8_t,21> code{};
    Require(shimImage.Read(method,code.data(),code.size()),"native_forwarding_read");
    // GetProperties loads the callable and nested public handle from the same
    // forwarding object and tail-calls CFG. Decode its offsets, never guess them.
    constexpr uint8_t first[]{0x48,0x8b,0x81},second[]{0x48,0x8b,0x89},tail[]{0x48,0xff,0x25};
    Require(!memcmp(code.data(),first,3) && !memcmp(code.data()+7,second,3)
        && !memcmp(code.data()+14,tail,3),"native_forwarding_shape");
    const auto callableOffset=provider_image::ReadU32(code.data()+3),handleOffset=provider_image::ReadU32(code.data()+10);
    Require(callableOffset>=0x20 && callableOffset<0x1000 && !(callableOffset%8)
        && handleOffset>=0x20 && handleOffset<0x1000 && !(handleOffset%8)
        && callableOffset!=handleOffset,"native_forwarding_offsets");
    shimImage.Target(method,{17,21,-1,3,0});
    const auto callable=Read<uintptr_t>(implementation+callableOffset);
    Binding result; result.fg=Owner(callable);
    Require(result.fg!=shim && callable==Export(result.fg,"xefgSwapChainGetProperties"),"native_forwarding_export");
    std::string failure;
    if (!FindCapacity(reinterpret_cast<const uint8_t*>(result.fg),result.site,failure)) throw std::runtime_error(failure);
    result.nested=reinterpret_cast<xefg_swapchain_handle_t>(Read<uintptr_t>(implementation+handleOffset));
    Require(Read<uint32_t>(uintptr_t(result.nested))==0x47466558,"native_nested_magic");
    const auto nativeImplementation=Read<uintptr_t>(uintptr_t(result.nested)+8),nativeTable=Read<uintptr_t>(nativeImplementation);
    Require(Owned(result.fg,nativeTable,0xb8,PAGE_READONLY),"native_dispatch_table");
    Image driver(result.fg);
    for (const auto [offset,expected] : {std::pair<uint32_t,uint32_t>{8,result.site.forwardProperties},
        {0x58,result.site.forwardCount},{0xa0,result.site.forwardParameters}})
    {
        const auto address=Read<uintptr_t>(nativeTable+offset);
        Require(Owner(address)==result.fg && Resolve(driver,uint32_t(address-uintptr_t(result.fg)))==expected,"native_dispatch_shape");
    }
    // These witnessed methods forward +10. The full allocation and setter
    // witnesses establish capacity +150, active +14c and pending +154.
    result.core=Read<uintptr_t>(nativeImplementation+0x10);
    const auto coreTable=Read<uintptr_t>(result.core);
    Require(coreTable==uintptr_t(result.fg)+result.site.coreTable
        && Owned(result.fg,coreTable,8,PAGE_READONLY),"native_core_owner");
    return result;
}
using Properties=decltype(&xefgSwapChainGetProperties);
using Parameters=decltype(&xefgSwapChainD3D12GetInitializationParameters);
struct Publication
{
    std::mutex mutex; HMODULE fg{},ll{}; uint32_t fgRva{},llRva{};
    const char* binding{""};
    bool gated{},applied{},poisoned{};
};
Publication& Published() { static auto* value=new Publication; return *value; }
}

bool FindCapacity(const uint8_t* base,CapacitySite& output,std::string& failure) noexcept
{
    try
    {
        failure.clear(); output={};
        Image image(reinterpret_cast<HMODULE>(const_cast<uint8_t*>(base)));
        const std::array<const xess_layout::Witness*,15> witnesses{&profile::capacity,&profile::flag,&profile::count,
            &profile::config,&profile::properties,&profile::allocate,&profile::normalize,&profile::set_count,
            &profile::forward_properties,&profile::forward_count,&profile::core_ctor,&profile::export_properties,
            &profile::export_parameters,&profile::unwrap,&profile::forward_parameters};
        std::array<std::vector<uint32_t>,15> candidates;
        for (size_t i=0;i<witnesses.size();++i)
        {
            if (!i)
            {
                auto bytes=std::vector<unsigned char>(profile::capacity_bytes,profile::capacity_bytes+sizeof(profile::capacity_bytes));
                bytes[14]=kGenerated;
                auto raised=profile::capacity; raised.bytes=bytes.data();
                auto sites=image.Find(profile::capacity),other=image.Find(raised);
                sites.insert(sites.end(),other.begin(),other.end());
                candidates[i]=std::move(sites);
            }
            else
            {
                candidates[i]=image.Find(*witnesses[i]);
            }
            Require(!candidates[i].empty(),("native_witness_"+std::to_string(i)+"_missing").c_str());
        }
        // Several tiny forwarding methods and TLS singleton accessors share a
        // shape. Admit only the unique connected allocation/properties/count
        // graph, rather than arbitrarily choosing the first byte match.
        bool changed=true;
        while (changed)
        {
            changed=false;
            for (const auto& link:profile::links)
            {
                auto& from=candidates[link.from]; auto& to=candidates[link.to];
                const auto target=[&](uint32_t start) { return Resolve(image,image.Target(start+link.offset,{1,5,-1,1,0})); };
                const auto oldFrom=from.size(),oldTo=to.size();
                std::erase_if(from,[&](uint32_t start) { return std::find(to.begin(),to.end(),target(start))==to.end(); });
                std::erase_if(to,[&](uint32_t end) { return std::none_of(from.begin(),from.end(),[&](uint32_t start) { return target(start)==end; }); });
                Require(!from.empty() && !to.empty(),"native_control_flow_link");
                changed|=oldFrom!=from.size() || oldTo!=to.size();
            }
        }
        std::array<uint32_t,15> found{};
        for (size_t i=0;i<candidates.size();++i)
        {
            Require(candidates[i].size()==1,("native_witness_"+std::to_string(i)+"_ambiguous").c_str());
            found[i]=candidates[i][0];
        }
        CapacitySite result; result.immediate=found[0]+14;
        result.config=image.Target(found[3],{44,48,-1,2,0});
        result.forwardProperties=found[8]; result.forwardCount=found[9];
        result.coreTable=image.Target(found[10],{41,45,-1,3,0});
        result.forwardParameters=found[14];
        const auto module=reinterpret_cast<HMODULE>(const_cast<uint8_t*>(base));
        Require(Resolve(image,uint32_t(Export(module,"xefgSwapChainGetProperties")-uintptr_t(module)))==found[11]
            && Resolve(image,uint32_t(Export(module,"xefgSwapChainD3D12GetInitializationParameters")-uintptr_t(module)))==found[12],
            "native_public_api_binding");
        output=result; return true;
    }
    catch (const std::exception& e) { failure=e.what(); return false; }
    catch (...) { failure="native_capacity_exception"; return false; }
}
bool FindXellGate(const uint8_t* base,uint32_t& immediate,std::string& failure) noexcept
{
    try
    {
        failure.clear(); immediate=0;
        const auto module=reinterpret_cast<HMODULE>(const_cast<uint8_t*>(base)); Image image(module);
        auto bytes=std::vector<unsigned char>(profile::ll_gate_bytes,profile::ll_gate_bytes+sizeof(profile::ll_gate_bytes));
        bytes[27]=kGenerated;
        auto raised=profile::ll_gate; raised.bytes=bytes.data();
        auto sites=image.Find(profile::ll_gate),other=image.Find(raised);
        sites.insert(sites.end(),other.begin(),other.end());
        Require(sites.size()==1,sites.empty() ? "native_xell_missing" : "native_xell_ambiguous");
        const auto exported=Export(module,"xellSetGeneratedFramesCount");
        Require(Resolve(image,uint32_t(exported-uintptr_t(module)))==sites[0],"native_xell_export_binding");
        immediate=sites[0]+27; return true;
    }
    catch (const std::exception& e) { failure=e.what(); return false; }
    catch (...) { failure="native_xell_exception"; return false; }
}
bool Raise(HMODULE module,uint32_t rva,uint8_t before,uint8_t after,std::string& failure) noexcept
{
    try
    {
        failure.clear(); Image image(module);
        Require(image.Range(rva,1,1),"native_write_bounds");
        auto* byte=reinterpret_cast<uint8_t*>(module)+rva;
        Require(Owned(module,uintptr_t(byte),1,PAGE_EXECUTE_READ),"native_write_owner");
        Require(Read<uint8_t>(uintptr_t(byte))==before,"native_write_expected");
        DWORD prior{};
        Require(VirtualProtect(byte,1,PAGE_EXECUTE_WRITECOPY,&prior)!=FALSE,"native_write_cow");
        const bool exchanged=prior==PAGE_EXECUTE_READ && uint8_t(_InterlockedCompareExchange8(
            reinterpret_cast<volatile char*>(byte),char(after),char(before)))==before;
        const bool flushed=exchanged && FlushInstructionCache(GetCurrentProcess(),byte,1);
        if (exchanged && !flushed) _InterlockedCompareExchange8(reinterpret_cast<volatile char*>(byte),char(before),char(after));
        DWORD unused{};
        const bool restored=VirtualProtect(byte,1,prior,&unused)!=FALSE;
        Require(exchanged && flushed && restored && Owned(module,uintptr_t(byte),1,prior)
            && Read<uint8_t>(uintptr_t(byte))==after,"native_write_verification");
        return true;
    }
    catch (const std::exception& e) { failure=e.what(); return false; }
    catch (...) { failure="native_write_exception"; return false; }
}
bool SelectXeLL(HMODULE fg,HMODULE routeLl,Receipt& receipt,std::string& failure) noexcept
{
    // Public Create runs outside loader lock. Choose before either immediate
    // changes, and hold a reference across all verification/publication work.
    const DWORD incomingError=GetLastError();
    struct PreserveError { DWORD value; ~PreserveError() { SetLastError(value); } } preserve{incomingError};
    receipt.ll=nullptr; receipt.llRva=0; receipt.llBinding=""; receipt.llGated=false; receipt.igxellLoaded=false;
    receipt.llMajor=receipt.llMinor=receipt.llPatch=0; receipt.llPath.clear();
    try
    {
        failure.clear(); CapacitySite site;
        if (!FindCapacity(reinterpret_cast<const uint8_t*>(fg),site,failure)) throw std::runtime_error(failure);
        DriverChoice choice; Choose(choice);
        receipt.igxellLoaded=choice.igxell.module!=nullptr;
        Require(choice.libxell.module!=nullptr,"native_xell_not_loaded");
        Require(choice.versionKnown,"native_xell_version");
        receipt.llMajor=choice.version.major; receipt.llMinor=choice.version.minor; receipt.llPatch=choice.version.patch;
        receipt.llBinding=choice.Binding();
        if (choice.Binding()==std::string_view("igxell"))
        {
            // libxell wrapped a driver context: the driver calls igxell, which
            // must be the bound native FG's own sibling.
            receipt.llPath=Path(choice.igxell.module).wstring();
            Require(SameFile(Path(fg).parent_path()/L"igxell.dll",receipt.llPath),"native_igxell_not_sibling");
            if (!FindXellGate(reinterpret_cast<const uint8_t*>(choice.igxell.module),receipt.llRva,failure))
                throw std::runtime_error("native_xell_validation_failed:"+failure);
            receipt.llGated=true;
            receipt.ll=choice.igxell.Release(); return true;
        }
        // The driver calls libxell with the caller's own context. Its gate is the
        // one the other-vendor unlock raises, mapped by the route's XeLL layout.
        receipt.llPath=Path(choice.libxell.module).wstring();
        Require(choice.libxell.module==routeLl,"native_xell_binding_not_route");
        const auto& layout=xess_layout::Get(choice.libxell.module,false);
        if (layout.addresses.contains(xess_host_unlock::kLlGate))
        {
            const auto compare=layout.Rva(xess_host_unlock::kLlGate);
            std::array<uint8_t,5> code{};
            Require(Image(choice.libxell.module).Read(compare,code.data(),code.size())
                && code[0]==0x83 && code[1]==0xfb && code[3]==0x76 && code[4]==0x07
                && (code[2]==kStockGenerated || code[2]==kGenerated),"native_libxell_gate_shape");
            receipt.llRva=compare+2; receipt.llGated=true;
        }
        // XeLL 1.2.x has no count gate; its witnessed setter must be unchanged.
        else layout.VerifyFunction(choice.libxell.module,xess_host_unlock::kLlUngatedSetter);
        receipt.ll=choice.libxell.Release(); return true;
    }
    catch (const std::exception& e) { failure=e.what(); return false; }
    catch (...) { failure="native_xell_selection_exception"; return false; }
}
const char* DriverXellBinding() noexcept
{
    try { DriverChoice choice; Choose(choice); return choice.Binding(); }
    catch (...) { return "unknown"; }
}
const char* PublishedXellBinding() noexcept
{
    auto& published=Published(); std::lock_guard lock(published.mutex);
    return published.applied ? published.binding : "";
}
bool Prepare(HMODULE shim,HMODULE routeLl,void* handle,Receipt& receipt,std::string& failure) noexcept
{
    try
    {
        failure.clear(); receipt={};
        auto& published=Published(); std::lock_guard lock(published.mutex);
        Require(!published.poisoned,"native_rollback_failed_restart_required");
        const auto binding=Bind(shim,handle);
        receipt.fg=binding.fg; receipt.fgRva=binding.site.immediate;
        xefg_swapchain_d3d12_init_params_t params{};
        const auto query=reinterpret_cast<Parameters>(Export(binding.fg,"xefgSwapChainD3D12GetInitializationParameters"));
        Require(query(binding.nested,&params)==XEFG_SWAPCHAIN_RESULT_ERROR_UNINITIALIZED,"native_context_not_cold");
        Require(Read<uint8_t>(uintptr_t(binding.fg)+binding.site.config+binding.site.flagOffset)==0,"intel_driver_override_active");
        if (!SelectXeLL(binding.fg,routeLl,receipt,failure)) throw std::runtime_error(failure);
        ModuleReference llReference{receipt.ll};
        const auto ll=llReference.module;
        const auto llRva=receipt.llRva;
        const bool gated=receipt.llGated;
        xefg_swapchain_properties_t properties{};
        const auto propertiesQuery=reinterpret_cast<Properties>(Export(binding.fg,"xefgSwapChainGetProperties"));
        Require(propertiesQuery(binding.nested,&properties)==XEFG_SWAPCHAIN_RESULT_SUCCESS,"native_properties_query");
        if (published.applied)
        {
            Require(published.fg==binding.fg && published.ll==ll && published.fgRva==binding.site.immediate
                && published.llRva==llRva && published.gated==gated && properties.maxSupportedInterpolations==kGenerated
                && (!gated || Read<uint8_t>(uintptr_t(ll)+llRva)==kGenerated),"native_publication_changed");
        }
        else
        {
            Require(properties.maxSupportedInterpolations==kStockGenerated
                && Read<uint8_t>(uintptr_t(binding.fg)+binding.site.immediate)==kStockGenerated
                && (!gated || Read<uint8_t>(uintptr_t(ll)+llRva)==kStockGenerated),"native_stock_state_unproven");
            Pin(shim); Pin(binding.fg); Pin(ll);
            Require(Owned(binding.fg,uintptr_t(binding.fg)+binding.site.immediate,1,PAGE_EXECUTE_READ)
                && (!gated || Owned(ll,uintptr_t(ll)+llRva,1,PAGE_EXECUTE_READ)),"native_pair_protection");
            bool fgRaised=false,llRaised=false;
            try
            {
                if (gated && !Raise(ll,llRva,kStockGenerated,kGenerated,failure)) throw std::runtime_error(failure);
                llRaised=gated;
                if (!Raise(binding.fg,binding.site.immediate,kStockGenerated,kGenerated,failure)) throw std::runtime_error(failure);
                fgRaised=true;
                Require(propertiesQuery(binding.nested,&properties)==XEFG_SWAPCHAIN_RESULT_SUCCESS
                    && properties.maxSupportedInterpolations==kGenerated,"native_capacity_readback");
            }
            catch (...)
            {
                std::string rollback;
                bool restored=true;
                // Also undo a changed byte whose write failed verification.
                if (fgRaised || Read<uint8_t>(uintptr_t(binding.fg)+binding.site.immediate)==kGenerated)
                    restored=Raise(binding.fg,binding.site.immediate,kGenerated,kStockGenerated,rollback) && restored;
                if (gated && (llRaised || Read<uint8_t>(uintptr_t(ll)+llRva)==kGenerated))
                    restored=Raise(ll,llRva,kGenerated,kStockGenerated,rollback) && restored;
                restored=restored && Read<uint8_t>(uintptr_t(binding.fg)+binding.site.immediate)==kStockGenerated
                    && Owned(binding.fg,uintptr_t(binding.fg)+binding.site.immediate,1,PAGE_EXECUTE_READ)
                    && (!gated || (Read<uint8_t>(uintptr_t(ll)+llRva)==kStockGenerated
                        && Owned(ll,uintptr_t(ll)+llRva,1,PAGE_EXECUTE_READ)));
                if (!restored)
                {
                    published.poisoned=true;
                    throw std::runtime_error("native_rollback_failed_restart_required");
                }
                throw;
            }
            published.fg=binding.fg; published.ll=ll; published.fgRva=binding.site.immediate; published.llRva=llRva;
            published.binding=receipt.llBinding; published.gated=gated; published.applied=true;
        }
        receipt.maximum=properties.maxSupportedInterpolations;
        return true;
    }
    catch (const std::exception& e) { failure=e.what(); return false; }
    catch (...) { failure="native_prepare_exception"; return false; }
}
bool Readback(HMODULE shim,void* handle,Receipt& receipt,std::string& failure) noexcept
{
    try
    {
        failure.clear(); receipt={};
        auto& published=Published(); std::lock_guard lock(published.mutex);
        const auto binding=Bind(shim,handle);
        xefg_swapchain_d3d12_init_params_t params{};
        const auto query=reinterpret_cast<Parameters>(Export(binding.fg,"xefgSwapChainD3D12GetInitializationParameters"));
        Require(query(binding.nested,&params)==XEFG_SWAPCHAIN_RESULT_SUCCESS,"native_init_readback");
        const auto capacity=Read<uint32_t>(binding.core+0x150),active=Read<uint32_t>(binding.core+0x14c),pending=Read<uint32_t>(binding.core+0x154);
        Require(capacity==params.maxInterpolatedFrames && capacity>0 && capacity<=kGenerated
            && active<=capacity && pending<=capacity,"native_allocation_readback");
        receipt={binding.fg,published.ll,binding.site.immediate,published.llRva,0,capacity,active,pending}; return true;
    }
    catch (const std::exception& e) { failure=e.what(); return false; }
    catch (...) { failure="native_readback_exception"; return false; }
}
bool Qualify(HMODULE shim,void* handle,Receipt& receipt,std::string& failure) noexcept
{
    try
    {
        failure.clear(); receipt={}; const auto binding=Bind(shim,handle);
        receipt.fg=binding.fg; receipt.fgRva=binding.site.immediate; return true;
    }
    catch (const std::exception& e) { failure=e.what(); return false; }
    catch (...) { failure="native_qualification_exception"; return false; }
}
}
