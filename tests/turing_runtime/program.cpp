#include <filesystem>
#include <fstream>
#include <iostream>
#include "ampere_gpu.cpp"
using namespace ampere_gpu;
void Check(bool okay,const char* why) { if(!okay) throw std::runtime_error(why); }
void LogAudit(const wchar_t* s) { std::wcout<<s<<L'\n'; }
const void* observedImage=nullptr;
unsigned forwarded=0;
uint32_t reportedSm=75;
uint32_t __fastcall MockSm(void*) { return reportedSm; }
int __fastcall MockKernel(void*,const void* image,uint32_t,const char*,uint32_t flags,void** result,uint32_t x,uint32_t y,uint32_t z,uint64_t shared)
{
    Check(flags==1&&x==2&&y==3&&z==4&&shared==5,"CreateKernel ABI arguments preserved");
    observedImage=image;++forwarded;*result=reinterpret_cast<void*>(0x1234);return 37;
}
unsigned protectionCall=0,failProtection=0;
BOOL WINAPI FailProtect(LPVOID p,SIZE_T n,DWORD mode,PDWORD old)
{
    if(++protectionCall==failProtection)return FALSE;
    return VirtualProtect(p,n,mode,old);
}
int wmain(int argc,wchar_t** argv)
{
    try
    {
        if(argc!=3) throw std::runtime_error("usage: TuringProgramAudit provider output-folder");
        HMODULE module=LoadLibraryExW(argv[1],nullptr,DONT_RESOLVE_DLL_REFERENCES);
        uint32_t size=0; Check(module&&ImageSize(module,size),"map provider without executing code");
        SetLogCallback(&LogAudit); gTargetSm.store(75);
        for(const char* type:{"NGXCubinD3D12","NGXCubinVulkan","Flatten_EndpointDL1Net","Flatten_EndpointDL1Net_89_PTX","Flatten_EndpointDL2Net","Flatten_EndpointDL2Net_89_PTX"})
            std::cout<<type<<" vt=0x"<<std::hex<<TuringVtable(module,type)-reinterpret_cast<uintptr_t>(module)<<std::dec<<'\n';
        for(const char* name:{"Flatten_EndpointDL1Net","Flatten_EndpointDL2Net"})
        {
            auto va=TuringVtable(module,name),vb=TuringVtable(module,(std::string(name)+"_89_PTX").c_str());
            Check(va&&vb,"paired network vtables");
            std::vector<TuringInstruction>a,b;std::vector<TuringLoad>la,lb;
            auto da=TuringDecode(module,ReadU64(reinterpret_cast<uint8_t*>(va+32)),a);
            auto db=TuringDecode(module,ReadU64(reinterpret_cast<uint8_t*>(vb+32)),b);
            auto aa=TuringLoads(module,a,la,false),bb=TuringLoads(module,b,lb,true);
            std::cout<<name<<" decode="<<da<<","<<db<<" instructions="<<a.size()<<","<<b.size()<<" loads="<<aa<<":"<<la.size()<<","<<bb<<":"<<lb.size()<<" equal="<<TuringSameLoaders(a,b,la,lb)<<'\n';
        }
        auto network=PrepareTuringNetwork(module);
        if(!network)
        {
            const auto vt=TuringVtable(module,"NGXCubinD3D12");
            std::vector<TuringInstruction> query;
            std::cout<<"SM query graph="<<TuringDecodeSm(module,ReadU64(reinterpret_cast<uint8_t*>(vt+8*8)),query)<<'\n';
            for(const auto& ins:query)
            {
                printf("SM opcode=%02x/%02x imm=%08x flags=%08x bytes=",ins.decoded.opcode,ins.decoded.opcode2,ins.decoded.imm.imm32,ins.decoded.flags);
                for(auto byte:ins.bytes)printf("%02x",byte);puts("");
            }
        }
        Check(network&&network->images.size()==39,"discover and translate all 39 neural-network programs");
        Check(TuringNetworkCurrent(*network,false),"network publication originals");
        Check(WriteTuringNetwork(network,true,&VirtualProtect)&&TuringNetworkCurrent(*network,true),"network publication and protections");
        Check(WriteTuringNetwork(network,false,&VirtualProtect)&&TuringNetworkCurrent(*network,false),"network rollback");
        for (unsigned fail : {1u,3u,5u,7u})
        {
            protectionCall=0;failProtection=fail;
            Check(!WriteTuringNetwork(network,true,&FailProtect),"publication failure closes admission");
            Check(TuringNetworkCurrent(*network,false),"failed publication restores all originals and protections");
        }
        auto& kernel=network->changes[0];const auto realKernel=kernel.original;
        const auto realSm=network->smQueries[0];
        Check(PublishPointer(network->cubinVtables[0]+8*8,realSm,reinterpret_cast<uintptr_t>(&MockSm)).success,"mock SM query fixture");
        network->smQueries[0]=reinterpret_cast<uintptr_t>(&MockSm);
        Check(PublishPointer(kernel.slot,realKernel,reinterpret_cast<uintptr_t>(&MockKernel)).success,"mock dispatch fixture");
        kernel.original=reinterpret_cast<uintptr_t>(&MockKernel);
        Check(WriteTuringNetwork(network,true,&VirtualProtect),"publish mock dispatch");
        uintptr_t self=network->cubinVtables[0];void* handle=nullptr;
        for(size_t i=0;i<network->images.size();++i)
        {
            const auto& image=network->images[i];
            Check(TuringCreateKernel(&self,reinterpret_cast<void*>(image.source),image.sourceBytes,network->names[i].data(),1,&handle,2,3,4,5)==37
                &&observedImage==image.allocation&&handle==reinterpret_cast<void*>(0x1234),"exact lowered network image substituted");
        }
        const auto& first=network->images[0];const auto calls=forwarded;
        Check(TuringCreateKernel(&self,reinterpret_cast<void*>(first.source),first.sourceBytes+1,network->names[0].data(),1,&handle,2,3,4,5)==-1,"wrong source size rejected");
        Check(TuringCreateKernel(&self,reinterpret_cast<void*>(first.source),first.sourceBytes,"wrong_entry",1,&handle,2,3,4,5)==-1,"wrong entry rejected");
        uint64_t unknown=0;
        Check(TuringCreateKernel(&self,&unknown,8,"unknown",1,&handle,2,3,4,5)==-1&&forwarded==calls,"unknown program rejected before forwarding");
        reportedSm=86;
        Check(TuringCreateKernel(&self,reinterpret_cast<void*>(first.source),first.sourceBytes,network->names[0].data(),1,&handle,2,3,4,5)==-1&&forwarded==calls,"a different instance SM cannot use the Turing program");reportedSm=75;
        Check(WriteTuringNetwork(network,false,&VirtualProtect),"mock dispatch rollback");
        Check(PublishPointer(kernel.slot,kernel.original,realKernel).success,"restore provider dispatch fixture");kernel.original=realKernel;
        Check(PublishPointer(network->cubinVtables[0]+8*8,network->smQueries[0],realSm).success,"restore provider SM fixture");network->smQueries[0]=realSm;
        std::filesystem::path out(argv[2]); std::filesystem::create_directories(out);
        unsigned total=0;
        const auto base=reinterpret_cast<uintptr_t>(module);
        dlssg_provider_policy::VersionTriplet version{};
        Check(dlssg_provider_policy::ReadProviderVersion(argv[1],version),"provider version");
        const auto* profile=ProfileForVersion(version);Check(profile!=nullptr,"temporal profile");
        DescriptorDiscovery discovery{};
        const uintptr_t anchor=FindDescriptorEntry(module,size,*profile,&discovery);Check(anchor!=0,"registration-bound table");
        const uintptr_t table=anchor-profile->temporalSlot*sizeof(uintptr_t);
        uintptr_t temporal=0,mask=0;
        for(size_t i=0;i<discovery.registeredSlots;++i)
        {
            uintptr_t descriptor=0,image=0;Check(SafeRead(table+i*8,descriptor)&&SafeRead(descriptor+8,image),"descriptor source");
            if(i==profile->temporalSlot)temporal=image;
            if(OutputPullMaskSlot(*profile,i))mask=image;
        }
        for(uintptr_t p=base;p+16<base+size;++p)
        {
            uint32_t magic=0;
            if(!SafeRead(p,magic)||magic!=0xBA55ED50) continue;
            uint16_t header=0;uint64_t content=0;
            if(!SafeRead(p+6,header)||!SafeRead(p+8,content)||header<16||content>kMaximumFatbinBytes)continue;
            const uint64_t bytes=header+content;
            if(!TuringOwned(module,p,static_cast<size_t>(bytes)))continue;
            Sm89PtxMetadata meta{};
            if(!FindUniqueSm89Ptx(p,bytes,meta))continue;
            const size_t cap=meta.outerHeaderSize+meta.imageHeaderSize+meta.rawPtxBytes*16+4096;
            std::vector<uint8_t> data(cap),scratch(meta.compressedSize);
            uint32_t written=0;StripFailure why{};
            Check(BuildAmpereSm86Fatbin(p,bytes,meta,data.data(),cap,scratch.data(),scratch.size(),written,why,p==temporal?profile:nullptr,p==mask),"translate complete provider inventory including variable temporal positions and OutputPull mask");
            const size_t offset=meta.outerHeaderSize+meta.imageHeaderSize;
            const auto name="program-"+std::to_string(total)+"-"+std::to_string(p-base);
            std::ofstream f(out/(name+".ptx"),std::ios::binary);
            std::string text(reinterpret_cast<char*>(data.data()+offset),written-offset);
            while(!text.empty()&&text.back()=='\0')text.pop_back(); f<<text;f.close();
            Check(f.good(),"write assembler input");
            ++total;p+=static_cast<uintptr_t>(bytes)-1;
        }
        Check(total>=65,"complete program inventory includes network, table and auxiliary programs");
        std::cout<<"TURING_PROGRAM_AUDIT_PASS programs="<<total<<" network="<<network->images.size()<<'\n';return 0;
    }
    catch(const std::exception& e){std::cerr<<"TURING_AUDIT_FAIL: "<<e.what()<<'\n';return 1;}
}
