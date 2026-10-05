#include "game_profile.h"
#include "checked_memory.h"
#include <algorithm>
#include <cstring>
namespace witcher_dots::profile {
namespace {
thread_local bool writableRequested{};
BOOL WINAPI GateProtect(void* address,SIZE_T size,DWORD desired,DWORD* previous) {
    if(desired==PAGE_EXECUTE_READWRITE||desired==PAGE_EXECUTE_WRITECOPY)writableRequested=true;
    return VirtualProtect(address,size,desired,previous);
}
template<size_t N> bool Matches(const uint8_t* base,uint32_t rva,const std::array<uint8_t,N>& expected) {
    std::array<uint8_t,N> observed{};
    return CopyChecked(observed.data(),base+rva,N)&&observed==expected;
}
bool StringAt(const uint8_t* base,uint64_t rva,uint32_t imageSize,const char* expected) {
    const size_t length=strlen(expected)+1;char observed[64]{};
    return length<=sizeof(observed)&&rva<imageSize&&imageSize-rva>=length
        &&CopyChecked(observed,base+rva,length)&&!memcmp(observed,expected,length);
}
// Pointers in the variable are relocated in the loaded game and still at the
// preferred base in an unrelocated image mapping (the CPU harness).
bool ConfigVarMatches(const uint8_t* base,const IMAGE_NT_HEADERS64& nt,const ConfigVar& var) {
    std::array<uint64_t,7> q{};
    if(!CopyChecked(q.data(),base+var.object,sizeof(q)))return false;
    const uint64_t loaded=reinterpret_cast<uintptr_t>(base),preferred=nt.OptionalHeader.ImageBase;
    const uint64_t delta=q[4]==loaded+var.value?loaded:q[4]==preferred+var.value?preferred:0;
    if(!delta||var.value!=var.object+0x30||q[3]!=var.flags||q[1]<delta||q[2]<delta
        ||!StringAt(base,q[1]-delta,nt.OptionalHeader.SizeOfImage,var.group)
        ||!StringAt(base,q[2]-delta,nt.OptionalHeader.SizeOfImage,var.name))return false;
    std::array<uint8_t,7> compare{};int32_t displacement{};
    if(!CopyChecked(compare.data(),base+var.reader,compare.size())||compare[0]!=var.opcode||compare[1]!=0x3d||compare[6]!=0)return false;
    memcpy(&displacement,compare.data()+2,4);
    return static_cast<int64_t>(var.reader)+7+displacement==var.value;
}
}
bool KnownDeviceCaller(const void* caller,const void* image) noexcept {
    for(const auto& game:kProfiles)for(const auto ret:game.deviceReturns)
        if(caller==static_cast<const uint8_t*>(image)+ret)return true;
    return false;
}
bool ValidateMapped(HMODULE image,const GameProfile& game,std::string& error) {
    const auto* base=reinterpret_cast<const uint8_t*>(image);
    IMAGE_DOS_HEADER dos{};IMAGE_NT_HEADERS64 nt{};
    if(!CopyChecked(&dos,base,sizeof(dos))||dos.e_magic!=IMAGE_DOS_SIGNATURE||dos.e_lfanew<=0||dos.e_lfanew>0x100000
        ||!CopyChecked(&nt,base+dos.e_lfanew,sizeof(nt))||nt.Signature!=IMAGE_NT_SIGNATURE
        ||nt.FileHeader.Machine!=IMAGE_FILE_MACHINE_AMD64||nt.OptionalHeader.Magic!=IMAGE_NT_OPTIONAL_HDR64_MAGIC
        ||nt.OptionalHeader.SizeOfImage<game.minImageSize) {error="live game PE layout mismatch";return false;}
    for(const auto& entry:game.entries)if(!Matches(base,entry.rva,entry.before)) {error="game hook entry changed";return false;}
    for(const auto& entry:game.gates)if(!Matches(base,entry.rva,entry.before)) {error="game hair gate changed";return false;}
    if(!Matches(base,game.hairWriterRva,game.hairWriter)||!Matches(base,game.ordinaryWriterRva,game.ordinaryWriter)) {
        error="game instance-mask layout changed";return false;
    }
    for(const uint32_t ret:game.deviceReturns) {
        std::array<uint8_t,6> call{};int32_t displacement{};
        if(!CopyChecked(call.data(),base+ret-6,call.size())||call[0]!=0xff||call[1]!=0x15) {error="renderer device caller changed";return false;}
        memcpy(&displacement,call.data()+2,4);
        if(static_cast<int64_t>(ret)+displacement!=game.deviceImport) {error="renderer device import target changed";return false;}
    }
    // Each hooked hair operation is called from exactly this site and to the
    // entry the hook is installed on (prebuild, build, instance copy).
    if(game.hairCalls[0].target!=game.entries[1].rva||game.hairCalls[1].target!=game.entries[2].rva
        ||game.hairCalls[2].target!=game.entries[3].rva) {error="hair operation profile inconsistent";return false;}
    for(const auto [ret,target]:game.hairCalls) {
        std::array<uint8_t,5> call{};int32_t displacement{};
        if(!CopyChecked(call.data(),base+ret-5,call.size())||call[0]!=0xe8) {error="hair operation caller changed";return false;}
        memcpy(&displacement,call.data()+1,4);
        if(static_cast<int64_t>(ret)+displacement!=target) {error="hair operation target changed";return false;}
    }
    if(game.gates.empty()||game.owner.size<std::max({game.owner.scratch,game.owner.blas,game.owner.positions,game.owner.indices})+8) {
        error="game profile incomplete";return false;
    }
    if(!ConfigVarMatches(base,nt,game.ptEnable)||!ConfigVarMatches(base,nt,game.ptHairQuality)) {error="game path-traced hair setting layout changed";return false;}
    return true;
}
protected_pointer::PublishResult ReplaceGate(uintptr_t base,const Patch& patch,bool rollback) noexcept {
    const uintptr_t address=base+patch.rva;
    const auto& before=rollback?patch.after:patch.before;
    const auto& after=rollback?patch.before:patch.after;
    std::array<uint8_t,8> observed{};DWORD protection{};
    if(address<base||address%8||!CopyChecked(observed.data(),reinterpret_cast<const void*>(address),8)
        ||observed!=before||!protected_pointer::QueryProtection(address,protection,8)||protection!=PAGE_EXECUTE_READ)return {};
    // Windows reports WRITECOPY on a fresh image page and READWRITE after its
    // first private write. Probe without writing, restore exactly, then let the
    // shared CAS publisher enforce the actual temporary protection and flush.
    DWORD previous{},temporary{};
    VirtualProtect(reinterpret_cast<void*>(address),8,PAGE_EXECUTE_READWRITE,&previous);
    const bool writable=protected_pointer::QueryProtection(address,temporary,8)
        &&(temporary==PAGE_EXECUTE_READWRITE||temporary==PAGE_EXECUTE_WRITECOPY);
    const bool restored=protected_pointer::RestoreProtectionWithRetry(reinterpret_cast<void*>(address),8,protection,&VirtualProtect);
    if(!writable||!restored)return {};
    writableRequested=false;
    const auto result=protected_pointer::ReplaceProtectedBytes(address,before.data(),after.data(),8,protection,
        &GateProtect,&FlushInstructionCache,temporary);
    // The general publisher also permits idempotent publication. A private
    // gate must not adopt an identical value installed by an unowned writer.
    if(result.disposition==protected_pointer::PublishDisposition::ePublishedRestored&&!writableRequested)return {};
    return result;
}
bool PublishGates(uintptr_t base,std::span<const Patch> patches,std::string& error) {
    size_t owned=0;
    for(const auto& gate:patches) {
        const auto result=ReplaceGate(base,gate);
        if(result.disposition!=protected_pointer::PublishDisposition::ePublishedRestored) {
            bool restored=result.disposition==protected_pointer::PublishDisposition::eNotPublishedRestored
                ||result.disposition==protected_pointer::PublishDisposition::eRolledBackRestored;
            // Only completed publications establish our ownership. A failed
            // current attempt may have observed a competing writer's identical
            // bytes; neither readback nor replacementWasPublished owns them.
            for(size_t j=0;j<owned;++j) {
                const auto& prior=patches[j];
                restored=ReplaceGate(base,prior,true).disposition
                    ==protected_pointer::PublishDisposition::ePublishedRestored&&restored;
            }
            error=restored?"hair gate publication failed; rollback verified":"hair gate publication failed; rollback indeterminate";
            return false;
        }
        ++owned;
    }
    return true;
}
}
