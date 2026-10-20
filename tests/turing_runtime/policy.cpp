#include "turing_ptx.h"
#include "ampere_provider_arch.h"
#include "ampere_policy.h"
#include "gpu_selection_policy.h"
#include "streamline_ota_policy.h"
#include "xess_ui_state.h"
#include <cstdio>
#include <stdexcept>
int main()
{
    unsigned n=0;
    auto check=[&](bool b,const char* why) { ++n; if(!b) throw std::runtime_error(why); };
    try
    {
        using namespace ampere_provider_arch;
        for (unsigned i=0;i<32;++i)
            check(IsTuringRtx(0x160,i)==(i==2||i==4||i==6),"only TU102/104/106");
        check(!IsTuringRtx(0x170,2)&&!IsTuringRtx(0x190,4),"architecture identity");
        check(ampere_policy::AdapterMatches(42,42,1,7,5,1),"CUDA 7.5 match");
        check(!ampere_policy::AdapterMatches(42,43,1,7,5,1)&&!ampere_policy::AdapterMatches(42,42,2,7,5,1)
            &&!ampere_policy::AdapterMatches(42,42,1,7,5,3),"wrong LUID, duplicates, multi-node rejected");
        gpu_dispatch::Selection s;
        check(s.Observe(gpu_dispatch::Family::eTuring,42),"Turing selection");
        check(!s.Observe(gpu_dispatch::Family::eAmpere,42)&&s.family==gpu_dispatch::Family::eConflict,"family immutable");
        check(gpu_dispatch::AllowVulkanCreate(gpu_dispatch::Family::eTuring,true,true)
            &&!gpu_dispatch::AllowVulkanCreate(gpu_dispatch::Family::eTuring,true,false),"Vulkan Turing Create retains adapter gate");
        check(gpu_dispatch::AllowVulkanEvaluate(gpu_dispatch::Family::eTuring,true,true)
            &&!gpu_dispatch::AllowVulkanEvaluate(gpu_dispatch::Family::eTuring,true,false),"Vulkan Turing Evaluate retains adapter gate");
        using namespace streamline_ota_policy;
        check(InventoryGpu(true,1,0x160,true)==Gpu::eTuring && InventoryGpu(true,2,0x160,true)==Gpu::eUnknown
            && InventoryGpu(true,1,0x160,false)==Gpu::eUnknown,"OTA identity");
        check(Apply(0,true,true,true,Gpu::eTuring,Gpu::eUnknown).reason==Reason::eSelectedTuring,"selected OTA");
        check(Apply(0,true,true,true,Gpu::eUnknown,Gpu::eTuring).reason==Reason::eEarlyTuring,"early OTA");
        check(!Apply(0,true,true,true,Gpu::eConflict,Gpu::eTuring).allowOtaForced,"conflict OTA closed");
        check(xess_ui::DlssMfgFamily(4),"menu offers DLSS controls to Turing");
        std::string p=".version 8.0\n.target sm_89\n{ cvt.rn.f16x2.f32 %r1,%f2,%f3; max.f16x2 %r4,%r5,%r6; }";
        turing_ptx::Counts c{};
        check(turing_ptx::Lower(p,&c)&&c.pack==1&&c.minmax==1,"half conversions");
        check(p.find("cvt.rn.f16.f32 %mfg_t1,%f2")!=std::string::npos
            &&p.find("cvt.rn.f16.f32 %mfg_t0,%f3")!=std::string::npos,"high/low conversion order");
        std::string m=".target sm_89\nmma.sync.aligned.m16n8k16.row.col.f16.f16.f16.f16 {%r0,%r1},{%r2,%r3,%r4,%r5},{%r6,%r7},{%r8,%r9};";
        check(turing_ptx::Lower(m,&c)&&c.mma==1&&m.find("{%r2,%r3},{%r6}")!=std::string::npos
            &&m.find("{%r4,%r5},{%r7}")!=std::string::npos,"matrix fragment mapping");
        for(const char* bad:{"min.ftz.f16 %r1,%r2,%r3;","max.f16x2.NaN %r1,%r2,%r3;",
                "@%p1 max.f16 %h1,%h2,%h3;","cvt.rn.f16x2.f32 %r1,%f2,0f00000000;",
                "mma.sync.aligned.m16n8k16.row.col.f32.f16.f16.f32 {%f1,%f2}, {%r1,%r2}, {%r3}, {%f1,%f2};"})
        {
            std::string source=std::string(".target sm_89\n")+bad, before=source;
            check(!turing_ptx::Lower(source)&&source==before,"unknown/predicated form rejected without mutation");
        }
        printf("TURING_POLICY_PASS checks=%u\n",n); return 0;
    }
    catch(const std::exception& e){printf("FAIL %s after %u checks\n",e.what(),n); return 1;}
}
