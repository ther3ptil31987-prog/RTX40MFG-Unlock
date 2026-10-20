#define MFG_PUBLICATION_CANDIDATE 1
#define MFG_PUBLICATION_COMPLETE 1
#include "ampere_gpu.cpp"
#include "../ampere_audits/fixture.h"
using namespace ampere_gpu;
using publication_fixture::Check;
uintptr_t failAt=0;bool injected=false;
BOOL WINAPI Protection(LPVOID p,SIZE_T n,DWORD value,PDWORD old)
{
    if (!injected&&reinterpret_cast<uintptr_t>(p)==failAt
        &&(value==PAGE_WRITECOPY||value==PAGE_READWRITE)) { injected=true;return FALSE; }
    return VirtualProtect(p,n,value,old);
}
int wmain(int argc,wchar_t** argv)
{
    try
    {
        if(argc!=2)return 2;
        gTargetSm.store(75);
        for(unsigned mode=0;mode<4;++mode)
        {
            publication_fixture::Fixture f(argv[1]);
            auto& p=f.program;
            p.network=PrepareTuringNetwork(p.module);Check(p.network!=nullptr,"network prepared");
            std::vector<uint8_t> ptx,native;uint32_t size=0;Check(ImageSize(p.module,size),"provider bounds");
            FontProgram font;Check(PrepareFont(p.module,size,font,ptx,native),"SM75 font prepared");
            p.font->replacement.fill(0);std::copy(ptx.begin(),ptx.end(),p.font->replacement.begin());
            injected=false;
            failAt=mode==1?p.network->changes[1].slot:mode==2?p.font->pageStart:mode==3?p.table+3*8:0;
            const auto result=PublishProgram(p,f.payloads,&Protection);
            if(mode==0)
            {
                Check(result.success&&TuringNetworkCurrent(*p.network,true),"network, font and table commit together");
                Check(WriteTuringNetwork(p.network,false,&VirtualProtect),"restore network fixture");
            }
            else
            {
                Check(injected&&!result.success&&result.rollbackComplete,"failure rolls back the entire program transaction");
                Check(TuringNetworkCurrent(*p.network,false)&&FontCurrent(*p.font,false),"network and font originals restored");
            }
        }
        printf("TURING_COMBINED_PUBLICATION_PASS checks=%u\n",publication_fixture::checks);return 0;
    }
    catch(const std::exception& e){printf("FAIL %s\n",e.what());return 1;}
}
