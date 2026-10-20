#include "ampere_provider_arch.h"
#include <cstdio>
#include <stdexcept>
using namespace ampere_provider_arch;
void Check(bool b,const char* why){if(!b)throw std::runtime_error(why);}
int wmain(int argc,wchar_t** argv)
{
    try
    {
        if(argc!=2)return 2;
        HMODULE module=LoadLibraryExW(argv[1],nullptr,DONT_RESOLVE_DLL_REFERENCES);
        Check(module!=nullptr,"provider data mapping");
        Arm(0x160,8);Arm(0x160,7);Arm(0x170,0);Check(!Armed(),"GTX and GA100 excluded");
        Check(Declare(module)==Outcome::eNotArmed,"no declaration without RTX identity");
        Arm(0x160,2);Check(Armed()&&Read().gpuArchitecture==0x160,"Turing armed");
        auto* code=reinterpret_cast<uint8_t*>(GetProcAddress(module,kArchitectureExport));
        Check(ArchitectureExportIs(code,0x190),"source Ada declaration");
        Check(Declare(module)==Outcome::eDeclared&&ArchitectureExportIs(code,0x160),"Turing declaration published");
        Check(Read().lastVulkan==VulkanOutcome::eDeclared,"Vulkan requirements declared");
        Check(Declare(module)==Outcome::eAlreadyDeclared&&Read().lastVulkan==VulkanOutcome::eAlreadyDeclared,"idempotent declaration");
        MEMORY_BASIC_INFORMATION m{};Check(VirtualQuery(code,&m,sizeof(m))==sizeof(m)&&m.Protect==PAGE_EXECUTE_READ,"code protection restored");
        puts("TURING_DECLARATION_PASS");return 0;
    }
    catch(const std::exception& e){printf("FAIL %s\n",e.what());return 1;}
}
