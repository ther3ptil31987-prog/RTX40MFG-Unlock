#include "game_discovery.h"
#include "checked_memory.h"
#include "shaders.h"
#include "../third_party/minhook/src/hde/hde64.h"
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <functional>
#include <memory>
#include <mutex>
#include <span>
#include <string_view>
#include <vector>
namespace witcher_dots::profile {
namespace {
// Every structural fact below was measured on builds 25575366 and 25773555
// and agrees with the exact 25646871 profile; a build that differs in any of
// them is not admitted.
//
// NVAPI interface IDs of the wrappers the game links statically: the hair
// operations DOTS hooks and the capability query it calls.
constexpr uint32_t kCapsInterface=0x85a6c2a0,kPrebuildInterface=0x8d025b77,kBuildInterface=0xe24ead45;
constexpr uint8_t kLssCaps=6; // NVAPI_D3D12_RAYTRACING_CAPS_TYPE_LINEAR_SWEPT_SPHERES
// The hair builder (all of its contiguous unwind fragments) with external
// call/jump targets, RIP-relative and non-stack 32-bit displacements masked:
// the engine moves code, globals and object fields between updates.
constexpr uint32_t kBuilderSize=0x72e;
constexpr std::array<uint8_t,32> kBuilderShape{
    0x8e,0x49,0x87,0x5d,0x54,0xb5,0xba,0xa9,0x1e,0x41,0x14,0x32,0xee,0xba,0xac,0x32,
    0x4a,0x5c,0xfe,0x1b,0xe6,0x54,0x32,0x58,0x58,0x3c,0x18,0xef,0x50,0xc6,0x4e,0xa1};
// The hair owner offsets the builder addresses, relative to the first (its
// scratch buffer pointer), and the readable owner span DOTS reads.
constexpr std::array<uint32_t,11> kOwnerShape{0,8,0x10,0x18,0x28,0x30,0x74,0x78,0x7c,0x80,0x84};
constexpr uint32_t kOwnerSize=0x90;
// REDengine config variables the path-traced-hair predicate reads.
constexpr char kPathTracer[]="Rendering/RT/PathTracer";
constexpr uint64_t kPtEnableFlags=0x100,kPtHairQualityFlags=0x101;

struct Image {
    const uint8_t* base{};uint32_t size{};uint64_t preferred{};uint32_t stamp{};
    uint32_t text0{},text1{};
    struct Range {uint32_t begin,end;bool writable;char name[9];};
    std::vector<Range> data; // readable non-executable sections (initialized part)
    std::span<const IMAGE_RUNTIME_FUNCTION_ENTRY> functions;
    IMAGE_DATA_DIRECTORY imports{};
    bool Has(int64_t rva,uint64_t n) const noexcept {return rva>=0&&static_cast<uint64_t>(rva)<=size&&n<=size-static_cast<uint64_t>(rva);}
    bool Code(int64_t rva,uint64_t n) const noexcept {return rva>=text0&&static_cast<uint64_t>(rva)<=text1&&n<=text1-static_cast<uint64_t>(rva);}
    int32_t I32(uint32_t rva) const noexcept {int32_t v;memcpy(&v,base+rva,4);return v;}
    uint32_t U32(uint32_t rva) const noexcept {uint32_t v;memcpy(&v,base+rva,4);return v;}
    uint64_t U64(uint32_t rva) const noexcept {uint64_t v;memcpy(&v,base+rva,8);return v;}
    // An absolute pointer in the image: relocated in the loaded game, still at
    // the preferred base in an unrelocated mapping (the CPU harness).
    int64_t Rva(uint64_t pointer) const noexcept {
        const uint64_t loaded=reinterpret_cast<uintptr_t>(base);
        if(pointer>=loaded&&pointer-loaded<size)return static_cast<int64_t>(pointer-loaded);
        if(pointer>=preferred&&pointer-preferred<size)return static_cast<int64_t>(pointer-preferred);
        return -1;
    }
};
bool Committed(const uint8_t* p,size_t n) noexcept {
    while(n) {
        MEMORY_BASIC_INFORMATION m{};
        if(!VirtualQuery(p,&m,sizeof(m))||m.State!=MEM_COMMIT||!m.Protect||(m.Protect&(PAGE_NOACCESS|PAGE_GUARD)))return false;
        const auto* end=static_cast<const uint8_t*>(m.BaseAddress)+m.RegionSize;
        const size_t step=std::min<size_t>(n,static_cast<size_t>(end-p));
        p+=step;n-=step;
    }
    return true;
}
bool Parse(HMODULE module,Image& im,std::string& error) {
    const auto* base=reinterpret_cast<const uint8_t*>(module);
    IMAGE_DOS_HEADER dos{};IMAGE_NT_HEADERS64 nt{};
    if(!base||!CopyChecked(&dos,base,sizeof(dos))||dos.e_magic!=IMAGE_DOS_SIGNATURE||dos.e_lfanew<=0||dos.e_lfanew>0x100000
        ||!CopyChecked(&nt,base+dos.e_lfanew,sizeof(nt))||nt.Signature!=IMAGE_NT_SIGNATURE
        ||nt.FileHeader.Machine!=IMAGE_FILE_MACHINE_AMD64||nt.OptionalHeader.Magic!=IMAGE_NT_OPTIONAL_HDR64_MAGIC
        ||nt.OptionalHeader.NumberOfRvaAndSizes<=IMAGE_DIRECTORY_ENTRY_EXCEPTION) {error="game PE layout unreadable";return false;}
    im.base=base;im.size=nt.OptionalHeader.SizeOfImage;im.preferred=nt.OptionalHeader.ImageBase;im.stamp=nt.FileHeader.TimeDateStamp;
    const size_t first=static_cast<size_t>(dos.e_lfanew)+offsetof(IMAGE_NT_HEADERS64,OptionalHeader)+nt.FileHeader.SizeOfOptionalHeader;
    for(WORD i=0;i<nt.FileHeader.NumberOfSections;++i) {
        IMAGE_SECTION_HEADER s{};
        if(!CopyChecked(&s,base+first+i*sizeof(s),sizeof(s))) {error="game section table unreadable";return false;}
        const uint64_t begin=s.VirtualAddress,end=begin+s.Misc.VirtualSize;
        if(!(s.Characteristics&IMAGE_SCN_MEM_READ)||!s.Misc.VirtualSize)continue;
        if(end>im.size||!Committed(base+begin,s.Misc.VirtualSize)) {error="game section not readable";return false;}
        char name[9]{};memcpy(name,s.Name,8);
        if(s.Characteristics&IMAGE_SCN_MEM_EXECUTE) {
            if(!strcmp(name,".text")) {im.text0=static_cast<uint32_t>(begin);im.text1=static_cast<uint32_t>(end);}
        } else if(s.SizeOfRawData) {
            Image::Range range{static_cast<uint32_t>(begin),static_cast<uint32_t>(begin+std::min(s.Misc.VirtualSize,s.SizeOfRawData)),
                (s.Characteristics&IMAGE_SCN_MEM_WRITE)!=0,{}};
            memcpy(range.name,name,sizeof(range.name));
            im.data.push_back(range);
        }
    }
    // hde64 and the scans read up to 16 bytes past an instruction start.
    if(im.text1<=im.text0+64||!im.Has(im.text1,16)) {error="game code section missing";return false;}
    const auto& exceptions=nt.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION];
    if(!exceptions.Size||exceptions.Size%sizeof(IMAGE_RUNTIME_FUNCTION_ENTRY)||!im.Has(exceptions.VirtualAddress,exceptions.Size)
        ||!Committed(base+exceptions.VirtualAddress,exceptions.Size)) {error="game unwind table unreadable";return false;}
    im.functions={reinterpret_cast<const IMAGE_RUNTIME_FUNCTION_ENTRY*>(base+exceptions.VirtualAddress),exceptions.Size/sizeof(IMAGE_RUNTIME_FUNCTION_ENTRY)};
    if(nt.OptionalHeader.NumberOfRvaAndSizes>IMAGE_DIRECTORY_ENTRY_IMPORT)im.imports=nt.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    return true;
}
// Unwind entry containing an address (the table is sorted by begin address).
const IMAGE_RUNTIME_FUNCTION_ENTRY* Lookup(const Image& im,uint32_t rva) noexcept {
    const auto it=std::upper_bound(im.functions.begin(),im.functions.end(),rva,
        [](uint32_t value,const IMAGE_RUNTIME_FUNCTION_ENTRY& entry) {return value<entry.BeginAddress;});
    if(it==im.functions.begin())return nullptr;
    const auto& entry=*(it-1);
    return rva<entry.EndAddress?&entry:nullptr;
}
// Start of the function an address belongs to, following chained unwind
// fragments to their primary entry (0: none).
uint32_t Primary(const Image& im,uint32_t rva) noexcept {
    const auto* found=Lookup(im,rva);
    if(!found)return 0;
    IMAGE_RUNTIME_FUNCTION_ENTRY entry=*found;
    for(int depth=0;depth<32;++depth) {
        const uint32_t unwind=entry.UnwindData;
        if(!im.Has(unwind,4))return 0;
        if(!((im.base[unwind]>>3)&UNW_FLAG_CHAININFO))return entry.BeginAddress;
        const uint32_t chained=unwind+4+2*((im.base[unwind+2]+1u)&~1u);
        if(!im.Has(chained,sizeof(entry)))return 0;
        memcpy(&entry,im.base+chained,sizeof(entry));
    }
    return 0;
}
// End of a function's primary range and the fragments that directly follow it.
uint32_t FunctionEnd(const Image& im,uint32_t begin) noexcept {
    const auto* entry=Lookup(im,begin);
    if(!entry||entry->BeginAddress!=begin)return 0;
    uint32_t end=entry->EndAddress;
    for(const auto* next=entry+1;next<im.functions.data()+im.functions.size()&&next->BeginAddress==end
        &&Primary(im,next->BeginAddress)==begin;++next)end=next->EndAddress;
    return end;
}
std::vector<uint32_t> FindAll(const Image& im,uint32_t begin,uint32_t end,std::span<const uint8_t> pattern) {
    std::vector<uint32_t> out;
    const auto* first=im.base+begin;const auto* last=im.base+end;
    const std::boyer_moore_horspool_searcher searcher(pattern.begin(),pattern.end());
    for(auto at=first;at<last;) {
        const auto found=searcher(at,last).first;
        if(found==last)break;
        out.push_back(static_cast<uint32_t>(found-im.base));at=found+1;
    }
    return out;
}
std::vector<uint32_t> FindCode(const Image& im,std::span<const uint8_t> pattern) {return FindAll(im,im.text0,im.text1-16,pattern);}
// Sections that hold no absolute pointers, strings or shaders DOTS looks for.
bool Metadata(const Image::Range& range) noexcept {
    return !strcmp(range.name,".pdata")||!strcmp(range.name,".reloc")||!strcmp(range.name,".rsrc");
}
// Offsets in the code section where `value` occurs.
template<class F> void EachCodeByte(const Image& im,uint8_t value,uint32_t tail,F&& visit) {
    const auto* p=im.base+im.text0;const auto* end=im.base+im.text1-tail;
    while(p<end) {
        p=static_cast<const uint8_t*>(memchr(p,value,static_cast<size_t>(end-p)));
        if(!p)break;
        visit(static_cast<uint32_t>(p-im.base));++p;
    }
}
template<size_t N> bool BytesAt(const Image& im,uint32_t rva,const std::array<uint8_t,N>& expected) noexcept {
    return im.Has(rva,N)&&!memcmp(im.base+rva,expected.data(),N);
}
// Return addresses of rel32 calls (e8) and jumps (e9) to each target, and of
// `call [rip+slot]` (ff 15) through one import slot.
struct Transfers {std::vector<uint32_t> calls,jumps;};
template<size_t N> std::array<Transfers,N> TransfersTo(const Image& im,const std::array<uint32_t,N>& targets,
    uint32_t importSlot=0,std::vector<uint32_t>* importCalls=nullptr) {
    std::array<Transfers,N> out{};
    for(const uint8_t opcode:{uint8_t{0xe8},uint8_t{0xe9}})EachCodeByte(im,opcode,5,[&](uint32_t at) {
        const int64_t target=static_cast<int64_t>(at)+5+im.I32(at+1);
        for(size_t i=0;i<N;++i)if(target==targets[i])(opcode==0xe8?out[i].calls:out[i].jumps).push_back(at+5);
    });
    if(importCalls)EachCodeByte(im,0xff,6,[&](uint32_t at) {
        if(im.base[at+1]==0x15&&static_cast<int64_t>(at)+6+im.I32(at+2)==importSlot)importCalls->push_back(at+6);
    });
    for(auto& transfers:out) {std::sort(transfers.calls.begin(),transfers.calls.end());std::sort(transfers.jumps.begin(),transfers.jumps.end());}
    return out;
}
// One decoded instruction: hde64 for legacy encodings, plus the VEX (AVX)
// encodings hde64 predates (the builder uses them; a VEX load of the LSS byte
// must not escape the census either).
struct Insn {
    unsigned length{},immediate{},displacement{};
    bool modrm{},sib{},relative{},vex{},rexW{},rex{},prefix66{};
    uint8_t mod{},reg{},rm{},base{},opcode{},opcode2{};
    int64_t imm{};int32_t disp{};
};
bool DecodeVex(const uint8_t* p,Insn& out) noexcept {
    unsigned map=1,at=2;bool b=false;
    if(p[0]==0xc4) {
        map=p[1]&0x1f;b=!(p[1]&0x20);out.rexW=(p[2]&0x80)!=0;at=3;
        if(map<1||map>3)return false;
    }
    out.vex=true;out.opcode=0x0f;out.opcode2=p[at++];
    if(map==1&&out.opcode2==0x77) {out.length=at;return true;} // vzeroupper / vzeroall
    const uint8_t modrm=p[at++];
    out.modrm=true;out.mod=modrm>>6;out.reg=(modrm>>3)&7;out.rm=modrm&7;
    unsigned base=out.rm;
    if(out.mod!=3&&out.rm==4) {out.sib=true;base=p[at++]&7;if(out.mod==0&&base==5)out.displacement=4;}
    if(out.mod==1)out.displacement=1;
    else if(out.mod==2||(out.mod==0&&out.rm==5))out.displacement=4;
    out.base=static_cast<uint8_t>(base|(b?8:0));
    if(out.displacement==4)memcpy(&out.disp,p+at,4);
    else if(out.displacement==1)out.disp=static_cast<int8_t>(p[at]);
    at+=out.displacement;
    const uint8_t op=out.opcode2;
    out.immediate=map==3||(map==1&&((op>=0x70&&op<=0x73)||op==0xc2||op==0xc4||op==0xc5||op==0xc6))?1:0;
    if(out.immediate)out.imm=p[at];
    out.length=at+out.immediate;
    return out.length<=15;
}
bool Decode(const uint8_t* p,Insn& out) noexcept {
    out=Insn{};
    if(p[0]==0xc4||p[0]==0xc5)return DecodeVex(p,out);
    hde64s hs{};const unsigned n=hde64_disasm(p,&hs);
    if((hs.flags&F_ERROR)||!n||n>15)return false;
    out.length=n;out.opcode=hs.opcode;out.opcode2=hs.opcode2;
    out.immediate=(hs.flags&F_IMM64)?8:(hs.flags&F_IMM32)?4:(hs.flags&F_IMM16)?2:(hs.flags&F_IMM8)?1:0;
    out.displacement=(hs.flags&F_DISP32)?4:(hs.flags&F_DISP16)?2:(hs.flags&F_DISP8)?1:0;
    out.modrm=(hs.flags&F_MODRM)!=0;out.sib=(hs.flags&F_SIB)!=0;out.relative=(hs.flags&F_RELATIVE)!=0;
    out.rexW=hs.rex_w!=0;out.rex=(hs.flags&F_PREFIX_REX)!=0;out.prefix66=(hs.flags&F_PREFIX_66)!=0;
    out.mod=hs.modrm_mod;out.reg=hs.modrm_reg;out.rm=hs.modrm_rm;
    out.base=static_cast<uint8_t>((out.sib?hs.sib_base:hs.modrm_rm)|(hs.rex_b<<3));
    out.disp=out.displacement==4?static_cast<int32_t>(hs.disp.disp32):out.displacement==1?static_cast<int8_t>(hs.disp.disp8):0;
    out.imm=out.immediate==4?static_cast<int32_t>(hs.imm.imm32):out.immediate==1?static_cast<int8_t>(hs.imm.imm8):
        out.immediate==2?static_cast<int16_t>(hs.imm.imm16):static_cast<int64_t>(hs.imm.imm64);
    return true;
}
bool RipRelative(const Insn& insn) noexcept {return insn.modrm&&!insn.sib&&insn.mod==0&&insn.rm==5&&insn.displacement==4;}
// The unique function containing `mov ecx, <NVAPI interface id>` for each id.
std::array<uint32_t,3> InterfaceWrappers(const Image& im,const std::array<uint32_t,3>& ids,const std::array<const std::array<uint8_t,16>*,3>& prologues) {
    std::array<uint32_t,3> function{};std::array<bool,3> ambiguous{};
    EachCodeByte(im,0xb9,5,[&](uint32_t at) {
        const uint32_t id=im.U32(at+1);
        for(size_t i=0;i<ids.size();++i)if(id==ids[i]) {
            const uint32_t owner=Primary(im,at);
            if(!owner||(function[i]&&owner!=function[i]))ambiguous[i]=true;
            function[i]=owner;
        }
    });
    for(size_t i=0;i<ids.size();++i)if(ambiguous[i]||!function[i]||!BytesAt(im,function[i],*prologues[i]))function[i]=0;
    return function;
}
// Hash of the builder with relocated fields masked, and the hair owner
// displacements it addresses (rcx at entry, rbx after `mov rbx,rcx`).
bool BuilderShape(const Image& im,uint32_t begin,uint32_t end,std::array<uint8_t,32>& shape,std::vector<uint32_t>& owner,std::string& error) {
    std::vector<std::byte> normalized;normalized.reserve(end-begin);
    for(uint32_t at=begin;at<end;) {
        Insn insn;
        if(!Decode(im.base+at,insn)||insn.length>end-at) {error="hair builder does not decode";return false;}
        const unsigned n=insn.length;
        uint8_t bytes[16]{};memcpy(bytes,im.base+at,n);
        if(insn.relative) {
            const int64_t target=static_cast<int64_t>(at)+n+insn.imm;
            if(target<begin||target>=end)memset(bytes+n-insn.immediate,0,insn.immediate);
        }
        if(insn.displacement==4) {
            const bool rip=RipRelative(insn);
            const bool stack=!rip&&insn.mod!=0&&(insn.base==4||insn.base==5);
            if(!stack) {
                if(!rip&&!insn.sib&&insn.mod==2&&(insn.base==1||insn.base==3))owner.push_back(static_cast<uint32_t>(insn.disp));
                memset(bytes+n-insn.immediate-4,0,4);
            }
        }
        // `add/sub r64,imm32` on an object pointer is a field offset too (the
        // frame size, `sub rsp,imm32`, stays exact).
        if(!insn.vex&&insn.opcode==0x81&&insn.mod==3&&(insn.reg==0||insn.reg==5)&&insn.rexW&&insn.immediate==4&&insn.base!=4)
            memset(bytes+n-4,0,4);
        const auto* first=reinterpret_cast<const std::byte*>(bytes);
        normalized.insert(normalized.end(),first,first+n);
        at+=n;
    }
    shape=Sha256(normalized);
    return true;
}
// An instruction with a RIP-relative memory operand.
struct RipOperand {uint32_t at,target;Insn insn;};
bool ByteSized(const Insn& insn) noexcept {
    if(insn.vex)return false;
    if(insn.opcode==0x0f)return insn.opcode2==0xb6||insn.opcode2==0xbe||(insn.opcode2>=0x90&&insn.opcode2<=0x9f)||insn.opcode2==0xb0||insn.opcode2==0xc0;
    switch(insn.opcode) {
    case 0x00:case 0x02:case 0x08:case 0x0a:case 0x10:case 0x12:case 0x18:case 0x1a:case 0x20:case 0x22:case 0x28:case 0x2a:
    case 0x30:case 0x32:case 0x38:case 0x3a:case 0x80:case 0x84:case 0x86:case 0x88:case 0x8a:case 0xc0:case 0xc6:case 0xd0:
    case 0xd2:case 0xf6:case 0xfe:return true;
    default:return false;
    }
}
unsigned AccessSize(const Insn& insn) noexcept {
    if(ByteSized(insn))return 1;
    if(insn.vex)return 32;
    if(insn.opcode==0x0f)return insn.opcode2==0xb7||insn.opcode2==0xbf?2:16;
    return insn.rexW?8:insn.prefix66?2:4;
}
bool ByteWrite(const Insn& insn) noexcept {
    return !insn.vex&&(insn.opcode==0x88||(insn.opcode==0xc6&&insn.reg==0)||(insn.opcode==0x0f&&insn.opcode2>=0x90&&insn.opcode2<=0x9f));
}
// Every instruction whose RIP-relative operand targets [lo,hi). Each start
// that decodes with its ModRM at the candidate byte is kept: a misaligned
// decode can only add an entry (and so fail discovery), never hide one.
std::vector<RipOperand> RipOperands(const Image& im,uint32_t lo,uint32_t hi) {
    std::vector<RipOperand> out;
    for(uint32_t p=im.text0+5;p+16<im.text1;++p) {
        if((im.base[p]&0xc7)!=0x05)continue;
        const int64_t reach=static_cast<int64_t>(p)+5+im.I32(p+1);
        if(reach+4<lo||reach>=hi)continue;
        const size_t first=out.size();
        for(uint32_t back=4;back>=1;--back) {
            const uint32_t start=p-back;Insn insn;
            if(!Decode(im.base+start,insn)||!RipRelative(insn)||start+insn.length-insn.immediate-5!=p)continue;
            // The REX-less tail of a REX-prefixed instruction is the same
            // instruction, not a second one.
            if(out.size()>first&&out.back().at==start-1&&im.base[start-1]>=0x40&&im.base[start-1]<=0x4f
                &&out.back().insn.length==insn.length+1)continue;
            const int64_t target=static_cast<int64_t>(start)+insn.length+insn.disp;
            if(target>=lo&&target<hi)out.push_back({start,static_cast<uint32_t>(target),insn});
        }
    }
    return out;
}
// Every aligned 8-byte data slot holding a pointer into the image.
template<class F> void DataPointers(const Image& im,F&& visit) {
    for(const auto& range:im.data) {
        if(Metadata(range))continue;
        for(uint32_t at=(range.begin+7)&~7u;at+8<=range.end;at+=8)
            if(const int64_t rva=im.Rva(im.U64(at));rva>=0)visit(at,rva);
    }
}
bool StringAt(const Image& im,int64_t rva,const char* expected) noexcept {
    const size_t length=strlen(expected)+1;
    return im.Has(rva,length)&&!memcmp(im.base+rva,expected,length);
}
// A NUL-terminated string that occurs once as a whole string.
uint32_t UniqueString(const Image& im,const char* text) {
    const auto* bytes=reinterpret_cast<const uint8_t*>(text);
    const std::span<const uint8_t> pattern(bytes,strlen(text)+1);
    uint32_t found=0;
    for(const auto& range:im.data) {
        if(range.writable||Metadata(range))continue;
        for(const uint32_t at:FindAll(im,range.begin,range.end,pattern)) {
            if(at>range.begin&&im.base[at-1]!=0)continue;
            if(found)return 0;
            found=at;
        }
    }
    return found;
}
// The config variable object among the slots pointing at its name (q[2]),
// with the REDengine layout ConfigVarMatches checks: group, flags, value at +0x30.
bool ConfigVariable(const Image& im,const std::vector<uint32_t>& slots,const char* label,uint64_t flags,uint8_t opcode,ConfigVar& out,std::string& error) {
    uint32_t object=0;int found=0;
    for(const uint32_t at:slots) {
        if(at<16||!im.Has(at-16,56))continue;
        const uint32_t candidate=at-16;
        if(im.Rva(im.U64(candidate+32))!=candidate+0x30||!StringAt(im,im.Rva(im.U64(candidate+8)),kPathTracer))continue;
        ++found;object=candidate;
    }
    if(found!=1) {error=std::string("config variable ")+label+(found?" ambiguous":" not found");return false;}
    if(im.U64(object+24)!=flags) {error=std::string("config variable ")+label+" flags changed";return false;}
    out={object,object+0x30,kPathTracer,label,flags,0,opcode};
    return true;
}
// A compare of each value with zero (cmp byte/dword [rip+value],0), as the
// renderer's path-traced-hair predicate does.
void ConfigReaders(const Image& im,ConfigVar& first,ConfigVar& second) {
    EachCodeByte(im,0x3d,6,[&](uint32_t p) {
        if(p<=im.text0)return;
        const uint32_t at=p-1;
        if(im.base[at+6]!=0)return;
        const int64_t target=static_cast<int64_t>(at)+7+im.I32(at+2);
        for(ConfigVar* var:{&first,&second})
            if(!var->reader&&im.base[at]==var->opcode&&target==var->value)var->reader=at;
    });
}
// The D3D12CreateDevice import slot (by name, from whichever DLL supplies it).
uint32_t DeviceImport(const Image& im) {
    if(!im.imports.Size||!im.Has(im.imports.VirtualAddress,im.imports.Size))return 0;
    uint32_t slot=0;
    for(uint32_t at=im.imports.VirtualAddress;im.Has(at,sizeof(IMAGE_IMPORT_DESCRIPTOR));at+=sizeof(IMAGE_IMPORT_DESCRIPTOR)) {
        IMAGE_IMPORT_DESCRIPTOR d{};memcpy(&d,im.base+at,sizeof(d));
        if(!d.Name)break;
        if(!d.OriginalFirstThunk)continue;
        for(uint32_t i=0;im.Has(d.OriginalFirstThunk+8ull*i,8)&&i<0x10000;++i) {
            const uint64_t thunk=im.U64(d.OriginalFirstThunk+8*i);
            if(!thunk)break;
            if(thunk>>63||!StringAt(im,static_cast<int64_t>(thunk)+2,"D3D12CreateDevice"))continue;
            if(slot)return 0;
            slot=d.FirstThunk+8*i;
        }
    }
    return slot;
}
// The executable's VS_FIXEDFILEINFO (for the label only).
void FileVersion(const Image& im,uint32_t& ms,uint32_t& ls) {
    const std::array<uint8_t,4> signature{0xbd,0x04,0xef,0xfe};
    for(const auto& range:im.data)if(!strcmp(range.name,".rsrc"))for(const uint32_t at:FindAll(im,range.begin,range.end,signature))
        if(!(at&3)&&im.Has(at,16)&&im.U32(at+4)==0x10000) {ms=im.U32(at+8);ls=im.U32(at+12);return;}
}
bool Fail(std::string& error,const char* text) {error=text;return false;}
}

bool Discover(HMODULE image,Discovered& out,std::string& error) noexcept try {
    out=Discovered{};
    LARGE_INTEGER started{},frequency{};QueryPerformanceCounter(&started);QueryPerformanceFrequency(&frequency);
    size_t phase=0;
    const auto Mark=[&] {
        LARGE_INTEGER now{};QueryPerformanceCounter(&now);
        if(frequency.QuadPart>0&&phase<out.phases.size())out.phases[phase++]=static_cast<uint32_t>((now.QuadPart-started.QuadPart)*1000000/frequency.QuadPart);
    };
    Image im;
    if(!Parse(image,im,error))return false;
    auto& p=out.profile;
    const auto& reference=kProfiles.back(); // 25646871: hook prologues and instance-mask writers
    // 1. The NVAPI wrappers: capability query, prebuild info and AS build.
    const auto wrappers=InterfaceWrappers(im,{kCapsInterface,kPrebuildInterface,kBuildInterface},
        {&reference.entries[4].before,&reference.entries[1].before,&reference.entries[2].before});
    const uint32_t caps=wrappers[0],prebuild=wrappers[1],build=wrappers[2];
    if(!caps||!prebuild||!build)return Fail(error,"NVAPI raytracing wrappers not found");
    Mark();
    // 2. memcpy (the TLAS instance copy) and the two instance-mask writers.
    const auto copies=FindCode(im,reference.entries[3].before);
    const auto hairWriters=FindCode(im,reference.hairWriter),ordinaryWriters=FindCode(im,reference.ordinaryWriter);
    if(copies.size()!=1||hairWriters.size()!=1||ordinaryWriters.size()!=1)return Fail(error,"instance copy or mask writer not unique");
    const uint32_t copy=copies[0],hairWriter=hairWriters[0];
    p.hairWriterRva=hairWriter;p.hairWriter=reference.hairWriter;
    p.ordinaryWriterRva=ordinaryWriters[0];p.ordinaryWriter=reference.ordinaryWriter;
    const auto* copyEntry=Lookup(im,copy);
    if(!copyEntry||copyEntry->BeginAddress!=copy)return Fail(error,"instance copy is not a function entry");
    Mark();
    // 3. The hair builder: the only caller of prebuild and of build.
    const uint32_t deviceImport=DeviceImport(im);
    if(!deviceImport)return Fail(error,"D3D12CreateDevice import not found");
    std::vector<uint32_t> deviceCalls;
    const auto calls=TransfersTo(im,std::array<uint32_t,4>{prebuild,build,caps,copy},deviceImport,&deviceCalls);
    if(calls[0].calls.size()!=1||calls[1].calls.size()!=1||!calls[0].jumps.empty()||!calls[1].jumps.empty())
        return Fail(error,"hair prebuild/build callers not unique");
    const uint32_t builder=Primary(im,calls[0].calls[0]-5);
    if(!builder||Primary(im,calls[1].calls[0]-5)!=builder||!BytesAt(im,builder,reference.entries[0].before))
        return Fail(error,"hair builder not found");
    const uint32_t builderEnd=FunctionEnd(im,builder);
    if(builderEnd-builder!=kBuilderSize)return Fail(error,"hair builder size changed");
    std::vector<uint32_t> owner;
    if(!BuilderShape(im,builder,builderEnd,out.builderShape,owner,error))return false;
    if(out.builderShape!=kBuilderShape)return Fail(error,"hair builder code changed");
    std::sort(owner.begin(),owner.end());owner.erase(std::unique(owner.begin(),owner.end()),owner.end());
    if(owner.size()!=kOwnerShape.size())return Fail(error,"hair owner layout changed");
    for(size_t i=0;i<owner.size();++i)if(owner[i]-owner[0]!=kOwnerShape[i])return Fail(error,"hair owner layout changed");
    p.owner={owner[0],owner[0]+8,owner[0]+0x18,owner[0]+0x28,owner[0]+kOwnerSize};
    const std::array<uint32_t,5> entries{builder,prebuild,build,copy,caps};
    for(size_t i=0;i<entries.size();++i) {p.entries[i].rva=entries[i];memcpy(p.entries[i].before.data(),im.base+entries[i],16);}
    Mark();
    // 4. The instance copy call of the hair-mask writer.
    const uint32_t writer=Primary(im,hairWriter);uint32_t copyReturn=0;
    for(const uint32_t ret:calls[3].calls)if(writer&&Primary(im,ret-5)==writer) {
        if(copyReturn)return Fail(error,"hair instance copy call not unique");
        copyReturn=ret;
    }
    if(!copyReturn||copyReturn<=hairWriter||copyReturn-hairWriter>0x200)return Fail(error,"hair instance copy call not found");
    p.hairCalls={{{calls[0].calls[0],prebuild},{calls[1].calls[0],build},{copyReturn,copy}}};
    Mark();
    // 5. The LSS capability byte: the type-6 query result the caps routine
    //    stores (`and al,1; mov [lss],al`, `mov [lss],0` on failure).
    uint32_t initializer=0,lss=0;int lssQueries=0;
    for(const uint32_t ret:calls[2].calls) {
        const uint32_t function=Primary(im,ret-5);
        if(!function||(initializer&&function!=initializer))return Fail(error,"LSS capability query callers changed");
        initializer=function;
        const std::array<uint8_t,5> type{0xba,kLssCaps,0,0,0};
        if(ret<32||std::search(im.base+ret-29,im.base+ret-5,type.begin(),type.end())==im.base+ret-5)continue;
        ++lssQueries;
        for(uint32_t at=ret;at<ret+0x60&&!lss;++at)
            if(im.base[at]==0x24&&im.base[at+1]==0x01&&im.base[at+2]==0x88&&im.base[at+3]==0x05)
                lss=static_cast<uint32_t>(static_cast<int64_t>(at)+8+im.I32(at+4));
        bool cleared=false;
        for(uint32_t at=ret;at<ret+0x60&&!cleared;++at)
            cleared=im.base[at]==0xc6&&im.base[at+1]==0x05&&im.base[at+6]==0&&static_cast<int64_t>(at)+7+im.I32(at+2)==lss;
        if(!cleared)lss=0;
    }
    if(lssQueries!=1||!lss||im.Code(lss,1)||!im.Has(lss,4))return Fail(error,"LSS capability byte not found");
    out.lssByte=lss;
    Mark();
    // 6. Every access to the capability block around the LSS byte. A build
    //    that gates hair on the byte anywhere else is not admitted (the sites
    //    below are still resolved so the harness can compare them).
    constexpr uint32_t kBlock=64;
    std::string shape;
    const auto Shape=[&shape](const char* text) {if(shape.empty())shape=text;};
    std::vector<uint32_t> writers;std::vector<RipOperand> reads;
    uint32_t getter=0,rtao=0;int getters=0,rtaos=0;std::vector<std::pair<uint32_t,uint32_t>> blockGetters; // (function, block)
    for(const auto& op:RipOperands(im,lss-kBlock,lss+4)) {
        const auto& insn=op.insn;
        if(insn.opcode==0x8d&&!insn.vex) {
            // Address taken: only `lea rax,[block]; ret` getters whose callers
            // read other flags are accounted for (checked below).
            if(op.target>lss)continue;
            if(insn.length!=7||im.base[op.at]!=0x48||im.base[op.at+1]!=0x8d||im.base[op.at+2]!=0x05||im.base[op.at+7]!=0xc3) {
                Shape("LSS capability block address taken outside the known shape");continue;
            }
            blockGetters.push_back({op.at,op.target});continue;
        }
        if(op.target==lss+3) {
            if(insn.length==7&&im.base[op.at]==0x44&&im.base[op.at+1]==0x38&&im.base[op.at+2]==0x35&&im.base[op.at+7]==0x74) {rtao=op.at;++rtaos;}
            continue;
        }
        if(op.target>lss)continue;
        if(op.target<lss) {
            if(op.target+AccessSize(insn)>lss)Shape("wide access overlaps the LSS capability byte");
            continue;
        }
        if(ByteWrite(insn)) {if(const uint32_t function=Primary(im,op.at))writers.push_back(function);continue;}
        reads.push_back(op);
    }
    for(const auto& op:reads) {
        const auto& insn=op.insn;
        const bool movzx=!insn.vex&&insn.opcode==0x0f&&insn.opcode2==0xb6&&!insn.prefix66;
        if(movzx&&insn.length==7&&op.at%8==0&&insn.reg==0&&!insn.rex&&im.base[op.at+7]==0xc3) {getter=op.at;++getters;continue;}
        // Capability copies made where the byte is written (the device and
        // NVAPI pipeline-option setup); they read the real value.
        const uint32_t function=Primary(im,op.at);
        if(movzx&&function&&std::find(writers.begin(),writers.end(),function)!=writers.end())continue;
        char text[96]{};_snprintf_s(text,_TRUNCATE,"LSS capability read outside the known shape at 0x%x",op.at);
        Shape(text);
    }
    if(getters!=1)Shape("logical LSS getter not unique");
    if(rtaos!=1)Shape("RTAO hair variant gate not unique");
    if(blockGetters.size()>2) {Shape("LSS capability block getters changed");blockGetters.resize(2);}
    {
        std::array<uint32_t,2> targets{blockGetters.size()>0?blockGetters[0].first:0u,blockGetters.size()>1?blockGetters[1].first:0u};
        const auto users=TransfersTo(im,targets);
        for(size_t i=0;i<blockGetters.size();++i) {
            if(!users[i].jumps.empty())Shape("LSS capability block getter reached by a jump");
            for(const uint32_t ret:users[i].calls) {
                const uint32_t offset=im.base[ret+3];
                if(im.base[ret]!=0x0f||im.base[ret+1]!=0xb6||im.base[ret+2]!=0x40||offset>=kBlock
                    ||blockGetters[i].second+offset==lss)Shape("LSS capability read through the block getter");
            }
        }
    }
    Mark();
    // 7. No absolute pointer into the block or to its getters, and the two
    //    path-traced hair config variables.
    const uint32_t ptEnableName=UniqueString(im,"PTEnable"),ptHairName=UniqueString(im,"PTHairQualityMode");
    if(!ptEnableName||!ptHairName)return Fail(error,"path-traced hair setting names not found");
    bool pointer=false;std::vector<uint32_t> ptEnableSlots,ptHairSlots;
    DataPointers(im,[&](uint32_t at,int64_t rva) {
        if(rva>=static_cast<int64_t>(lss)-kBlock&&rva<=lss)pointer=true;
        for(const auto& [function,block]:blockGetters)if(rva==function)pointer=true;
        if(rva==ptEnableName)ptEnableSlots.push_back(at);
        if(rva==ptHairName)ptHairSlots.push_back(at);
    });
    if(pointer)Shape("LSS capability block referenced by pointer");
    if(!ConfigVariable(im,ptEnableSlots,"PTEnable",kPtEnableFlags,0x80,p.ptEnable,error)
        ||!ConfigVariable(im,ptHairSlots,"PTHairQualityMode",kPtHairQualityFlags,0x83,p.ptHairQuality,error))return false;
    ConfigReaders(im,p.ptEnable,p.ptHairQuality);
    if(!p.ptEnable.reader||!p.ptHairQuality.reader)return Fail(error,"path-traced hair setting readers not found");
    Mark();
    // 8. The renderer's two D3D12CreateDevice calls: in the function that
    //    also writes the LSS capability byte.
    std::vector<uint32_t> deviceReturns;
    std::sort(deviceCalls.begin(),deviceCalls.end());
    for(const uint32_t ret:deviceCalls) {
        const uint32_t function=Primary(im,ret-6);
        if(function&&std::find(writers.begin(),writers.end(),function)!=writers.end())deviceReturns.push_back(ret);
    }
    if(deviceReturns.size()!=2||Primary(im,deviceReturns[0])!=Primary(im,deviceReturns[1]))return Fail(error,"renderer device creation not found");
    p.deviceReturns={deviceReturns[0],deviceReturns[1]};p.deviceImport=deviceImport;
    Mark();
    // 9. The embedded hair shaders: exactly the known ClosestHitHair and
    //    PTHairPrepassRGS (two copies each).
    std::vector<std::pair<uint32_t,const ShaderIdentity*>> shaders;
    const std::array<uint8_t,4> dxbc{'D','X','B','C'};
    const std::string_view hairBuffers="HairVertexBuffers";
    for(const auto& range:im.data)if(!range.writable&&!Metadata(range))for(const uint32_t at:FindAll(im,range.begin,range.end,dxbc)) {
        if(at+28>range.end)continue;
        const uint32_t size=im.U32(at+24);
        if(size<32||size>range.end-at)continue;
        const std::string_view blob(reinterpret_cast<const char*>(im.base+at),size);
        if(blob.find(hairBuffers)==std::string_view::npos)continue;
        const std::span<const std::byte> bytes(reinterpret_cast<const std::byte*>(im.base+at),size);
        const ShaderIdentity* identity=nullptr;
        for(const auto& known:KnownShaders())if(known.size==size&&HashEquals(bytes,known.sha256))identity=&known;
        if(!identity)return Fail(error,"unknown hair shader (the game changed its hair shaders)");
        shaders.push_back({at,identity});
    }
    std::array<std::vector<std::pair<uint32_t,const ShaderIdentity*>>,2> kinds;
    for(const auto& shader:shaders)kinds[shader.second->kind==ShaderKind::ClosestHit?0:1].push_back(shader);
    for(const auto& kind:kinds)if(kind.size()!=2||kind[0].second!=kind[1].second)return Fail(error,"hair shader copies changed");
    p.shaders={kinds[0][0].first,kinds[0][1].first,kinds[1][0].first,kinds[1][1].first};
    p.shaderSizes={kinds[0][0].second->size,kinds[1][0].second->size};
    if(!shape.empty())return Fail(error,shape.c_str());
    Mark();
    // 10. The gates: RTAO keeps its regular variant (je -> jmp), the logical
    //     LSS getter reports LSS (last). Each is one aligned 8-byte publication.
    const uint32_t je=rtao+7,window=je&~7u;
    Patch rtaoGate{window,{},{}};memcpy(rtaoGate.before.data(),im.base+window,8);
    rtaoGate.after=rtaoGate.before;rtaoGate.after[je-window]=0xeb;
    Patch getterGate{getter,{},{0xb8,0x01,0x00,0x00,0x00,0xc3,0x90,0x90}};memcpy(getterGate.before.data(),im.base+getter,8);
    out.gates={rtaoGate,getterGate};
    FileVersion(im,out.versionMs,out.versionLs);
    // CDPR versions read major.minor.0.build, the build in all 32 low bits.
    _snprintf_s(out.label,_TRUNCATE,"Witcher 3 %u.%u.0.%u (hair code found by structure)",out.versionMs>>16,out.versionMs&0xffff,out.versionLs);
    p.label=out.label;p.exeSize=0;p.exeHash="not hashed (structure-discovered build)";
    p.minImageSize=im.size;
    p.gates=std::span<const Patch>(out.gates);
    LARGE_INTEGER finished{};QueryPerformanceCounter(&finished);
    if(frequency.QuadPart>0)out.microseconds=static_cast<uint32_t>((finished.QuadPart-started.QuadPart)*1000000/frequency.QuadPart);
    return true;
} catch(...) {error="hair code discovery failed";return false;}

namespace {
std::once_flag discoveryOnce;
std::atomic<const Discovered*> discovered{};
const std::string* discoveryFailure{};
}
const GameProfile* DiscoveredProfile(HMODULE image,std::string& error) noexcept {
    try {
        std::call_once(discoveryOnce,[image] {
            auto result=std::make_unique<Discovered>();std::string why;
            if(Discover(image,*result,why))discovered.store(result.release(),std::memory_order_release);
            else discoveryFailure=new std::string(why);
        });
    } catch(...) {error="hair code discovery failed";return nullptr;}
    if(const auto* found=discovered.load(std::memory_order_acquire))return &found->profile;
    error=discoveryFailure?*discoveryFailure:"hair code discovery failed";
    return nullptr;
}
const Discovered* DiscoveredSites() noexcept {return discovered.load(std::memory_order_acquire);}
}
