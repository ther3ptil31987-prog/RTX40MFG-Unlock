#include "../../source/native/xess/intel_native.h"
#include <Windows.h>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <array>

static unsigned checks{};
static void Check(bool ok,const char* what)
{
    ++checks;
    if (!ok) { std::fprintf(stderr,"FAIL %s\n",what); throw std::runtime_error(what); }
}
static void Write(HMODULE module,uint32_t rva,const void* data,size_t bytes)
{
    auto* at=reinterpret_cast<uint8_t*>(module)+rva; DWORD old{},unused{};
    Check(VirtualProtect(at,bytes,PAGE_EXECUTE_WRITECOPY,&old)!=FALSE,"fixture writable");
    memcpy(at,data,bytes);
    Check(FlushInstructionCache(GetCurrentProcess(),at,bytes)!=FALSE,"fixture flush");
    Check(VirtualProtect(at,bytes,old,&unused)!=FALSE,"fixture protect");
}
int wmain(int argc,wchar_t** argv)
{
    std::string failure;
    try
    {
        Check(argc==4,"native driver and modern SDK shim fixtures required");
        // No DllMain, imports or GPU APIs execute. Only the private test process
        // mapping changes; official files remain untouched.
        const auto fg=LoadLibraryExW(argv[1],nullptr,DONT_RESOLVE_DLL_REFERENCES);
        const auto ll=LoadLibraryExW(argv[2],nullptr,DONT_RESOLVE_DLL_REFERENCES);
        Check(fg && ll,"map fixtures without execution");
        intel_native::CapacitySite site; uint32_t gate{};
        Check(intel_native::FindCapacity(reinterpret_cast<uint8_t*>(fg),site,failure),"initial FG discovery");
        Check(site.immediate==0x152e3e && site.config==0x115bd40,"decoded capacity and config locations");
        Check(intel_native::FindXellGate(reinterpret_cast<uint8_t*>(ll),gate,failure),"initial XeLL discovery");
        Check(gate==0xcedb,"decoded XeLL exported count gate");
        Check(!intel_native::Raise(fg,site.immediate,4,5,failure),"wrong expected byte refused");
        Check(*(reinterpret_cast<uint8_t*>(fg)+site.immediate)==3,"failed write unchanged");
        Check(intel_native::Raise(ll,gate,3,5,failure),"raise XeLL with COW/CAS/protection readback");
        Check(intel_native::Raise(fg,site.immediate,3,5,failure),"raise native capacity");
        Check(intel_native::FindCapacity(reinterpret_cast<uint8_t*>(fg),site,failure),"rediscover raised native capacity");
        Check(intel_native::FindXellGate(reinterpret_cast<uint8_t*>(ll),gate,failure),"rediscover raised XeLL");
        Check(intel_native::Raise(fg,site.immediate,5,3,failure),"native rollback");
        Check(intel_native::Raise(ll,gate,5,3,failure),"XeLL rollback");
        Check(!intel_native::Raise(fg,0xfffffff0,3,5,failure),"out of image rejected");
        // A changed allocation bound must invalidate admission, even when the
        // capability getter still matches. This is the safety-critical graph.
        auto* code=reinterpret_cast<uint8_t*>(fg);
        const uint8_t changed=0x49,original=code[0x1b31c7];
        Write(fg,0x1b31c7,&changed,1);
        Check(!intel_native::FindCapacity(code,site,failure),"allocation contract mutation rejected");
        Write(fg,0x1b31c7,&original,1);
        Check(intel_native::FindCapacity(code,site,failure),"allocation restore accepted");
        // An in-image call to the wrong valid function is still a wrong owner
        // relationship; section membership alone is insufficient.
        int32_t prior{}; memcpy(&prior,code+0x152e43,4);
        const int32_t wrong=int32_t(0xc440)-int32_t(0x152e47);
        Write(fg,0x152e43,&wrong,4);
        Check(!intel_native::FindCapacity(code,site,failure),"wrong decoded call relationship rejected");
        Write(fg,0x152e43,&prior,4);
        Check(intel_native::FindCapacity(code,site,failure),"call relationship restore accepted");
        const uint8_t badCount=4;
        Write(fg,site.immediate,&badCount,1);
        Check(!intel_native::FindCapacity(code,site,failure),"foreign capability mutation rejected");
        const uint8_t stock=3;
        Write(fg,0x152e3e,&stock,1);
        Check(intel_native::FindCapacity(code,site,failure),"stock restored");
        DWORD old{},unused{};
        Check(VirtualProtect(code+site.immediate,1,PAGE_EXECUTE_READWRITE,&old)!=FALSE,"fixture protection mutation");
        Check(!intel_native::Raise(fg,site.immediate,3,5,failure),"foreign writable code protection rejected");
        Check(VirtualProtect(code+site.immediate,1,old,&unused)!=FALSE,"fixture protection restore");
        Check(!intel_native::FindCapacity(reinterpret_cast<uint8_t*>(ll),site,failure),"XeLL cannot identify as FG");
        Check(!intel_native::FindXellGate(code,gate,failure),"FG cannot identify as XeLL");
        Check(!intel_native::FindCapacity(nullptr,site,failure),"null image rejected");
        intel_native::Receipt receipt;
        Check(!intel_native::Prepare(fg,nullptr,nullptr,receipt,failure),"null context rejected before writes");
        std::array<uintptr_t,2> fakeHandle{};
        Check(!intel_native::Prepare(fg,nullptr,fakeHandle.data(),receipt,failure),"bad context magic rejected");
        fakeHandle[0]=0x47466558;
        Check(!intel_native::Prepare(fg,nullptr,fakeHandle.data(),receipt,failure),"bad implementation rejected without AV");
        // Bind a synthetic pair of public headers to the REAL mapped 3.0.2
        // forwarding table and native methods. No driver function is called.
        const auto shim=LoadLibraryExW(argv[3],nullptr,DONT_RESOLVE_DLL_REFERENCES);
        Check(shim!=nullptr,"map modern shim without DllMain");
        std::array<uintptr_t,2> nested{},core{};
        std::array<uintptr_t,0x200/8> forwarding{},implementation{};
        core[0]=uintptr_t(fg)+0x1118120;
        implementation[0]=uintptr_t(fg)+0x1119ca0;
        implementation[0x10/8]=uintptr_t(core.data());
        nested={0x47466558,uintptr_t(implementation.data())};
        forwarding[0]=uintptr_t(shim)+0x157bd98;
        forwarding[0x158/8]=uintptr_t(nested.data());
        forwarding[0x168/8]=uintptr_t(GetProcAddress(fg,"xefgSwapChainGetProperties"));
        fakeHandle[1]=uintptr_t(forwarding.data());
        Check(intel_native::Qualify(shim,fakeHandle.data(),receipt,failure),"modern shim -> native owner binding");
        Check(receipt.fg==fg && receipt.fgRva==0x152e3e,"actual forwarded provider qualified");
        forwarding[0x168/8]=uintptr_t(GetProcAddress(fg,"xefgSwapChainSetEnabled"));
        Check(!intel_native::Qualify(shim,fakeHandle.data(),receipt,failure),"wrong forwarded export refused");
        forwarding[0x168/8]=uintptr_t(GetProcAddress(fg,"xefgSwapChainGetProperties"));
        nested[0]=0;
        Check(!intel_native::Qualify(shim,fakeHandle.data(),receipt,failure),"nested magic refused");
        nested[0]=0x47466558;
        implementation[0]=uintptr_t(shim)+0x157bd98;
        Check(!intel_native::Qualify(shim,fakeHandle.data(),receipt,failure),"wrong native dispatch owner refused");
        implementation[0]=uintptr_t(fg)+0x1119ca0;
        core[0]=uintptr_t(shim)+0x157bd98;
        Check(!intel_native::Qualify(shim,fakeHandle.data(),receipt,failure),"wrong allocation-core owner refused");
        core[0]=uintptr_t(fg)+0x1118120;
        Check(intel_native::Qualify(shim,fakeHandle.data(),receipt,failure),"binding restores");
        FreeLibrary(shim);
        Check(code[0x152e3e]==3 && *(reinterpret_cast<uint8_t*>(ll)+0xcedb)==3,"all tests leave stock bytes");
        FreeLibrary(ll); FreeLibrary(fg);
        std::printf("PASS %u checks; real PE discovery/COW/rollback and fail-closed contracts; no GPU execution\n",checks);
        return 0;
    }
    catch (const std::exception& e) { std::fprintf(stderr,"%s: %s\n",e.what(),failure.c_str()); return 1; }
}
