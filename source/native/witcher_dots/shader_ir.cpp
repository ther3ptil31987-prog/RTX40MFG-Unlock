#include "shaders.h"
#include "rounded_normal_ir.h"
#include <algorithm>
#include <regex>
#include <set>
#include <sstream>
#include <stdexcept>
namespace witcher_dots {
namespace {
using RX=std::regex;
RX Pattern(const std::string& p) { return RX(p,std::regex_constants::ECMAScript); }
struct Located { size_t at{}, length{}; };
std::vector<Located> Lines(const std::string& s,const RX& pattern) {
    std::vector<Located> matches;
    size_t at=0;
    while(at<s.size()) {
        const auto end=s.find('\n',at);
        const auto n=(end==std::string::npos?s.size():end)-at;
        const auto line=s.substr(at,n);std::smatch m;
        if(std::regex_search(line,m,pattern)) matches.push_back({at+static_cast<size_t>(m.position()),static_cast<size_t>(m.length())});
        if(end==std::string::npos)break;at=end+1;
    }
    return matches;
}
std::string One(std::string s,const std::string& pattern,std::string_view replacement) {
    const auto span=pattern.find("[\\s\\S]*?");
    auto matches=Lines(s,Pattern(pattern.substr(0,span)));
    if(matches.size()!=1)throw std::runtime_error("shader profile marker mismatch: "+pattern);
    auto m=matches.front();
    if(span!=std::string::npos) {
        const auto tail=pattern.find("(?=",span);
        if(tail==std::string::npos||pattern.back()!=')')throw std::runtime_error("invalid shader range");
        const auto ends=Lines(s,Pattern(pattern.substr(tail+3,pattern.size()-tail-4)));
        if(ends.size()!=1||ends[0].at<=m.at)throw std::runtime_error("shader range mismatch");
        m.length=ends[0].at-m.at;
    }
    s.replace(m.at,m.length,replacement);
    return s;
}
std::string Match(const std::string& s,const std::string& pattern) {
    const auto matches=Lines(s,Pattern(pattern));
    if(matches.size()!=1)throw std::runtime_error("missing or ambiguous shader profile marker");
    return s.substr(matches[0].at,matches[0].length);
}
void ReplaceLiteral(std::string& s,std::string_view from,std::string_view to) {
    size_t at=0;
    while ((at=s.find(from,at))!=std::string::npos) { s.replace(at,from.size(),to);at+=to.size(); }
}
std::string Named(std::string s) {
    std::set<std::string> blocks{"0"};
    RX labels(R"(; <label>:(\d+))");
    for(auto it=std::sregex_iterator(s.begin(),s.end(),labels);it!=std::sregex_iterator();++it) blocks.insert((*it)[1]);
    RX refs(R"(%(\d+)\b)");
    std::string out;size_t pos=0;
    for(auto it=std::sregex_iterator(s.begin(),s.end(),refs);it!=std::sregex_iterator();++it) {
        const auto m=*it;out.append(s,pos,static_cast<size_t>(m.position())-pos);
        out+=(blocks.contains(m[1])?"%bb":"%r")+m[1].str();pos=static_cast<size_t>(m.position()+m.length());
    }
    out.append(s,pos,std::string::npos);
    std::istringstream lines(out);std::string line,result;
    while(std::getline(lines,line)) {
        line=std::regex_replace(line,Pattern(R"(^; <label>:(\d+)(.*)$)"),"bb$1:$2");
        line=std::regex_replace(line,Pattern(R"(^(define .* \{)$)"),"$1\nbb0:");
        result+=line+"\n";
    }
    return result;
}
void Prune(std::string& s) {
    std::istringstream input(s);std::string line,out;
    const RX symbol(R"((@[^ (]+)\()");
    while(std::getline(input,line)) {
        if (line.starts_with("declare ")) {
            std::smatch m;
            if (std::regex_search(line,m,symbol)) {
                const std::string needle=m[1].str()+"(";
                const auto at=s.find(needle);
                if (at!=std::string::npos&&s.find(needle,at+needle.size())==std::string::npos) continue;
            }
        }
        out+=line+"\n";
    }
    s=std::move(out);
}
// The anchors of every known build. Build 25575366 (5.0.0.1041720): the
// original LSS shaders. Build 25646871 (5.0.0.1044392): the prepass reads the
// strand u from committed barycentrics x instead of NVIDIA-extension calls,
// and both shaders compute the object-space normal without the old fallbacks.
constexpr ClosestAnchors kClosest25575366{"r9","r11","r12","r113","r150","r21",{121,125,129,133,137,141,145,149},{150,151,152,154,155,156},
    "27","28",{283,284,285},"r286"};
constexpr PrepassAnchors kPrepass25575366{"r211","r217","r220","r231","r238","r239","r251","r250",
    "r302","r314","r314","r313","r315","r348","r440","r480",{451,455,459,463,467,471,475,479},{501,502,503,504,505,506},
    "53","54",{632,633,634},"r635"};
constexpr ClosestAnchors kClosest25646871{"r9","r11","r12","r113","r150","r21",{121,125,129,133,137,141,145,149},{150,151,152,154,155,156},
    "27","28",{249,250,251},"r252"};
constexpr PrepassAnchors kPrepass25646871{"r211","r217","r220","r231","r238","r239","r240","r239",
    "r289","r290","r290","r289","r291","r324","r416","r456",{427,431,435,439,443,447,451,455},{468,469,470,471,472,473},
    "53","54",{562,563,564},"r565"};
constexpr std::array<ShaderIdentity,4> kShaders{{
    {ShaderKind::ClosestHit,8240,"4f2063aca18fdac330cdf5d54d52dfd22b620fc50f84184ef3adaefbcc206e3f",&kClosest25575366,nullptr},
    {ShaderKind::Prepass,63072,"3406beddeabcebcc372e10df365eac5ee03b7aa6e99aa4b1f5d4d252aae7ea6e",nullptr,&kPrepass25575366},
    {ShaderKind::ClosestHit,7804,"d6bdd62b710a5566db95e4277a7ba94a454ba04f15c65bcbff1c8c81974b4436",&kClosest25646871,nullptr},
    {ShaderKind::Prepass,62232,"736d1986356e2bf49c38fea948bbc38101a6fc67cd9c96d9c9380fd51de30acf",nullptr,&kPrepass25646871},
}};
// Renames %rN and %dots.flat.rN tokens of an inserted snippet (written in
// build 25575366 names) in one pass, so mapped names never chain.
std::string Renamed(std::string_view ir,const std::vector<std::pair<int,int>>& names) {
    const RX token(R"(%(dots\.flat\.)?r(\d+)\b)");
    std::string text(ir),out;size_t pos=0;
    for(auto it=std::sregex_iterator(text.begin(),text.end(),token);it!=std::sregex_iterator();++it) {
        const auto& m=*it;out.append(text,pos,static_cast<size_t>(m.position())-pos);
        int id=std::stoi(m[2].str());
        for(const auto& [from,to]:names)if(from==id) {id=to;break;}
        out+="%"+m[1].str()+"r"+std::to_string(id);pos=static_cast<size_t>(m.position()+m.length());
    }
    out.append(text,pos,std::string::npos);
    return out;
}
std::string Endpoints(bool pre,std::string global,const std::string& inst,const std::string& index,const std::string& non,const std::string& alias,
    const std::array<int,8>& ids) {
    std::string code=R"(  %dots.ptr = getelementptr inbounds [32 x %dx.types.Handle], [32 x %dx.types.Handle]* GLOBAL, i32 0, i32 INST, !dx.nonuniform !NON
  %dots.load = load %dx.types.Handle, %dx.types.Handle* %dots.ptr, align 4, !noalias !ALIAS
  %dots.handle0 = call %dx.types.Handle @dx.op.createHandleForLib.dx.types.Handle(i32 160, %dx.types.Handle %dots.load)
  %dots.handle = call %dx.types.Handle @dx.op.annotateHandle(i32 216, %dx.types.Handle %dots.handle0, %dx.types.ResourceProperties { i32 12, i32 16 })
  %dots.p0 = call %dx.types.ResRet.f32 @dx.op.rawBufferLoad.f32(i32 139, %dx.types.Handle %dots.handle, i32 INDEX, i32 0, i8 15, i32 4)
  %dots.next = add i32 INDEX, 1
  %dots.p1 = call %dx.types.ResRet.f32 @dx.op.rawBufferLoad.f32(i32 139, %dx.types.Handle %dots.handle, i32 %dots.next, i32 0, i8 15, i32 4)
)";
    ReplaceLiteral(code,"GLOBAL",global);ReplaceLiteral(code,"INST",inst);
    ReplaceLiteral(code,"INDEX",index);ReplaceLiteral(code,"NON",non);
    ReplaceLiteral(code,"ALIAS",alias);
    if(!pre) code="  %dots.inst = call i32 @dx.op.instanceID.i32(i32 141)\n"+code;
    for(size_t i=0;i<ids.size();++i)
        code+="  %r"+std::to_string(ids[i])+" = extractvalue %dx.types.ResRet.f32 %dots.p"+(i<4?"0":"1")+", "+std::to_string(i%4)+"\n";
    return code;
}
std::string QueryU(const char* query,const char* prim,const char* out,const char* tag) {
    std::string code=R"(  %TAG.bx = call float @dx.op.rayQuery_StateVector.f32(i32 194, i32 %QUERY, i8 0)
  %TAG.by = call float @dx.op.rayQuery_StateVector.f32(i32 194, i32 %QUERY, i8 1)
  %TAG.sum = fadd float %TAG.bx, %TAG.by
  %TAG.parity = and i32 %PRIM, 1
  %TAG.even = icmp eq i32 %TAG.parity, 0
  %OUT = select i1 %TAG.even, float %TAG.sum, float %TAG.by
)";
    ReplaceLiteral(code,"TAG",tag);ReplaceLiteral(code,"PRIM",prim);ReplaceLiteral(code,"OUT",out);ReplaceLiteral(code,"QUERY",query);
    return code;
}
void Normals(std::string& s,const std::array<int,3>& outputs,const char* marker,std::string_view rounded) {
    for(int id:outputs) {
        const auto name="%r"+std::to_string(id);
        auto phi=Match(s,"^  "+name+" = phi float.*$");
        ReplaceLiteral(phi,name+" =","%dots.flat.r"+std::to_string(id)+" =");
        s=One(std::move(s),"^  "+name+" = phi float.*$",phi);
    }
    const std::string pattern="^  %"+std::string(marker)+" =.*$";
    const std::string original=Match(s,pattern);
    s=One(std::move(s),pattern,std::string(rounded)+original);
}
std::vector<std::pair<int,int>> NameMap(const std::array<int,8>& fromExtracts,const std::array<int,8>& toExtracts,
    const std::array<int,6>& fromRay,const std::array<int,6>& toRay,const std::array<int,3>& fromNormals,const std::array<int,3>& toNormals) {
    std::vector<std::pair<int,int>> names;
    for(size_t i=0;i<8;++i)names.push_back({fromExtracts[i],toExtracts[i]});
    for(size_t i=0;i<6;++i)names.push_back({fromRay[i],toRay[i]});
    for(size_t i=0;i<3;++i)names.push_back({fromNormals[i],toNormals[i]});
    return names;
}
std::string N(const char* name) {return std::string("%")+name;}
}
std::span<const ShaderIdentity> KnownShaders() {return kShaders;}
const ShaderIdentity* FindShader(std::span<const std::byte> bytes,ShaderKind kind) {
    for(const auto& shader:kShaders)if(shader.kind==kind&&bytes.size()==shader.size&&HashEquals(bytes,shader.sha256))return &shader;
    return nullptr;
}
bool TranslateIr(std::string_view original,const ShaderIdentity& shader,std::string& translated,std::string& error) {
    try {
        const bool pre=shader.kind==ShaderKind::Prepass;
        if(pre?!shader.prepass:!shader.closest)throw std::runtime_error("shader anchors missing");
        std::string s=Named(std::string(original));
        auto global=Match(s,R"(^(@".*HairVertexBuffers[^"]*") =)");global.resize(global.size()-2);
        if (!pre) {
            const auto& a=*shader.closest;
            s=One(std::move(s),"^  "+N(a.prim)+" = call i32 @dx.op.primitiveIndex.i32.*$","  %dots.prim = call i32 @dx.op.primitiveIndex.i32(i32 161)\n  "
                +N(a.prim)+" = lshr i32 %dots.prim, 2");
            s=One(std::move(s),"^  "+N(a.attrU)+" = extractelement.*$","  %dots.bx = extractelement <2 x float> "+N(a.attrLoad)+", i32 0\n"
                "  %dots.by = extractelement <2 x float> "+N(a.attrLoad)+", i32 1\n"
                "  %dots.sum = fadd float %dots.bx, %dots.by\n  %dots.parity = and i32 %dots.prim, 1\n  %dots.even = icmp eq i32 %dots.parity, 0\n"
                "  "+N(a.attrU)+" = select i1 %dots.even, float %dots.sum, float %dots.by");
            s=One(std::move(s),"^  "+N(a.endpointsFrom)+" = [\\s\\S]*?(?=^  "+N(a.endpointsTo)+" =)",
                Endpoints(false,global,"%dots.inst",N(a.index),a.nonuniform,a.noalias,a.extracts));
            Normals(s,a.normals,a.normalMarker,Renamed(kClosestRoundedNormal,NameMap(kClosest25575366.extracts,a.extracts,
                kClosest25575366.ray,a.ray,kClosest25575366.normals,a.normals)));
        } else {
            const auto& a=*shader.prepass;
            // The game bridge must establish exclusive ownership of hair mask
            // 0x80 and the 32 descriptor slots before admitting this program.
            s=One(std::move(s),"^  "+N(a.hitFrom)+" = [\\s\\S]*?(?=^  br i1 "+N(a.hitBranch)+",)",
                "  %dots.id = call i32 @dx.op.rayQuery_StateScalar.i32(i32 208, i32 "+N(a.query)+")\n"
                "  %dots.triangle = icmp eq i32 "+N(a.candidateType)+", 1\n  %dots.slot = icmp ult i32 %dots.id, 32\n"
                "  %dots.valid = and i1 %dots.triangle, %dots.slot\n  "+N(a.hitBranch)+" = xor i1 %dots.valid, true\n");
            s=One(std::move(s),"^  "+N(a.alphaPrim)+" = call i32 @dx.op.rayQuery_StateScalar.i32.*$",
                "  %dots.palpha = call i32 @dx.op.rayQuery_StateScalar.i32(i32 210, i32 "+N(a.query)+")\n  "+N(a.alphaPrim)+" = lshr i32 %dots.palpha, 2");
            s=One(std::move(s),"^  "+N(a.alphaUFrom)+" = [\\s\\S]*?(?=^  "+N(a.alphaUTo)+" =)",QueryU(a.query,"dots.palpha",a.alphaU,"dots.ualpha"));
            s=One(std::move(s),"^  "+N(a.mainUFrom)+" = [\\s\\S]*?(?=^  "+N(a.mainUTo)+" =)","");
            s=One(std::move(s),"^  "+N(a.mainPrim)+" = call i32 @dx.op.rayQuery_StateScalar.i32.*$",
                "  %dots.pmain = call i32 @dx.op.rayQuery_StateScalar.i32(i32 210, i32 "+N(a.query)+")\n  "+N(a.mainPrim)+" = lshr i32 %dots.pmain, 2\n"
                    +QueryU(a.query,"dots.pmain",a.mainU,"dots.umain"));
            s=One(std::move(s),"^  "+N(a.endpointsFrom)+" = [\\s\\S]*?(?=^  "+N(a.endpointsTo)+" =)",
                Endpoints(true,global,N(a.instance),N(a.index),a.nonuniform,a.noalias,a.extracts));
            Normals(s,a.normals,a.normalMarker,Renamed(kPrepassRoundedNormal,NameMap(kPrepass25575366.extracts,a.extracts,
                kPrepass25575366.ray,a.ray,kPrepass25575366.normals,a.normals)));
        }
        Prune(s);
        if(s.find("call i32 @dx.op.bufferUpdateCounter")!=std::string::npos)
            throw std::runtime_error("untranslated NVIDIA extension sequence");
        translated=std::move(s);return true;
    } catch(const std::exception& e) { translated.clear();error=e.what();return false; }
}
}
