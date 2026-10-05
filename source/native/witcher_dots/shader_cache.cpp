#include "shaders.h"
#include "converter_source.h"
#include "checked_memory.h"
#include <bcrypt.h>
#include <filesystem>
#include <fstream>
#include <limits>
#include <sstream>
namespace witcher_dots {
using Microsoft::WRL::ComPtr;
namespace {
bool Result(IDxcOperationResult* r,ComPtr<IDxcBlob>& blob,std::string& error) {
    HRESULT status=E_FAIL;
    if(!r||FAILED(r->GetStatus(&status))) { error="DXC returned no operation status";return false; }
    if(FAILED(status)) {
        ComPtr<IDxcBlobEncoding> messages;
        r->GetErrorBuffer(&messages);
        error=messages?std::string(static_cast<const char*>(messages->GetBufferPointer()),messages->GetBufferSize()):"DXC operation failed";
        if(error.size()>2048)error.resize(2048);
        return false;
    }
    return SUCCEEDED(r->GetResult(&blob))&&blob;
}
}
std::array<uint8_t,32> Sha256(std::span<const std::byte> bytes) {
    std::array<uint8_t,32> output{};
    BCRYPT_ALG_HANDLE algorithm{};BCRYPT_HASH_HANDLE hash{};
    DWORD objectSize{},used{};std::vector<unsigned char> object;
    if(BCryptOpenAlgorithmProvider(&algorithm,BCRYPT_SHA256_ALGORITHM,nullptr,0)<0)return {};
    if(BCryptGetProperty(algorithm,BCRYPT_OBJECT_LENGTH,reinterpret_cast<PUCHAR>(&objectSize),sizeof(objectSize),&used,0)>=0) {
        object.resize(objectSize);
        if(BCryptCreateHash(algorithm,&hash,object.data(),objectSize,nullptr,0,0)>=0) {
            size_t at=0;bool ok=true;
            while(at<bytes.size()) {
                const auto n=static_cast<ULONG>(std::min<size_t>(bytes.size()-at,1u<<20));
                if(BCryptHashData(hash,reinterpret_cast<PUCHAR>(const_cast<std::byte*>(bytes.data()+at)),n,0)<0){ok=false;break;}
                at+=n;
            }
            if(!ok||BCryptFinishHash(hash,output.data(),static_cast<ULONG>(output.size()),0)<0)output={};
            BCryptDestroyHash(hash);
        }
    }
    BCryptCloseAlgorithmProvider(algorithm,0);return output;
}
bool HashEquals(std::span<const std::byte> bytes,const char* expected) {
    if(!expected||strlen(expected)!=64)return false;
    const auto hash=Sha256(bytes);
    for(size_t i=0;i<hash.size();++i) {
        const auto digit=[](char c)->int {return c>='0'&&c<='9'?c-'0':c>='a'&&c<='f'?c-'a'+10:c>='A'&&c<='F'?c-'A'+10:-1;};
        const int a=digit(expected[i*2]),b=digit(expected[i*2+1]);
        if(a<0||b<0||hash[i]!=(a*16+b))return false;
    }
    return true;
}
bool ShaderCache::Initialize(const std::wstring& directory,std::string& error) {
    const auto root=std::filesystem::path(directory);
    // Resolve dependencies relative to these absolute paths, never CWD.
    validatorModule_=LoadLibraryExW((root/L"dxil.dll").c_str(),nullptr,LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR|LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    compilerModule_=LoadLibraryExW((root/L"dxcompiler.dll").c_str(),nullptr,LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR|LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if(!compilerModule_||!validatorModule_) {error="game DXC/validator unavailable";return false;}
    const auto create=reinterpret_cast<DxcCreateInstanceProc>(GetProcAddress(compilerModule_,"DxcCreateInstance"));
    if(!create||FAILED(create(CLSID_DxcCompiler,IID_PPV_ARGS(&compiler_)))
        ||FAILED(create(CLSID_DxcLibrary,IID_PPV_ARGS(&library_)))
        ||FAILED(create(CLSID_DxcAssembler,IID_PPV_ARGS(&assembler_)))
        ||FAILED(create(CLSID_DxcOptimizer,IID_PPV_ARGS(&optimizer_)))
        ||FAILED(create(CLSID_DxcValidator,IID_PPV_ARGS(&validator_)))) {error="game DXC interface unavailable";return false;}
    // Compiler interfaces and transformed programs have process lifetime.
    HMODULE pin{};
    if(!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_PIN,reinterpret_cast<LPCWSTR>(create),&pin)
        ||pin!=compilerModule_) {error="cannot pin game compiler";return false;}
    const auto validatorEntry=GetProcAddress(validatorModule_,"DxcCreateInstance");
    if(!validatorEntry||!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_PIN,
        reinterpret_cast<LPCWSTR>(validatorEntry),&pin)||pin!=validatorModule_) {error="cannot pin game validator";return false;}
    return true;
}
bool ShaderCache::Disassemble(std::span<const std::byte> source,std::string& text,std::string& error) {
    if(!compiler_||source.size()>16u*1024*1024||source.empty()) {error="invalid shader input";return false;}
    DxcBuffer input{source.data(),source.size(),0};ComPtr<IDxcResult> result;ComPtr<IDxcBlob> blob;
    if(FAILED(compiler_->Disassemble(&input,IID_PPV_ARGS(&result)))||!Result(result.Get(),blob,error))return false;
    text.assign(static_cast<const char*>(blob->GetBufferPointer()),blob->GetBufferSize());
    while(!text.empty()&&text.back()=='\0')text.pop_back();return true;
}
bool ShaderCache::Validate(IDxcBlob* blob,std::vector<std::byte>& output,std::string& error) {
    ComPtr<IDxcOperationResult> result;ComPtr<IDxcBlob> validated;
    if(!validator_||FAILED(validator_->Validate(blob,0,&result))||!Result(result.Get(),validated,error))return false;
    const auto* first=static_cast<const std::byte*>(validated->GetBufferPointer());
    output.assign(first,first+validated->GetBufferSize());return true;
}
bool ShaderCache::Translate(std::span<const std::byte> source,ShaderKind kind,std::vector<std::byte>& output,std::string& error) {
    output.clear();
    const auto* shader=FindShader(source,kind);
    if(!shader) {error="unknown hair shader";return false;}
    std::string original,ir;
    if(!Disassemble(source,original,error)||!TranslateIr(original,*shader,ir,error))return false;
    ComPtr<IDxcBlobEncoding> input;ComPtr<IDxcOperationResult> result;ComPtr<IDxcBlob> blob,optimized;
    ComPtr<IDxcBlobEncoding> messages;
    // Assembling IR alone validates but does not finalize a library. The
    // removed extension sequences leave unused resource-handle loads which
    // the NVIDIA library compiler cannot consume. Run the game's standard
    // DXIL passes, then assemble and validate the resulting LLVM bitcode.
    const wchar_t* passes[]={L"-hlsl-dxilload",L"-instcombine",L"-simplifycfg",L"-dce",L"-hlsl-dxilfinalize"};
    if(FAILED(library_->CreateBlobWithEncodingFromPinned(ir.data(),static_cast<UINT32>(ir.size()),CP_UTF8,&input))
        ||!optimizer_) {error="DXIL optimizer unavailable";return false;}
    if(FAILED(optimizer_->RunOptimizer(input.Get(),passes,_countof(passes),&optimized,&messages))||!optimized) {
        error=messages?std::string(static_cast<const char*>(messages->GetBufferPointer()),messages->GetBufferSize()):"DXIL finalization failed";
        if(error.size()>2048)error.resize(2048);return false;
    }
    if(FAILED(assembler_->AssembleToContainer(optimized.Get(),&result))||!Result(result.Get(),blob,error))return false;
    if(!Validate(blob.Get(),output,error))return false;
    std::string finalIr;
    if(!Disassemble(output,finalIr,error)) {output.clear();return false;}
    const auto bindings=[](const std::string& s) {
        const auto begin=s.find("; Resource Bindings:"),end=s.find("target datalayout",begin);
        return begin!=std::string::npos&&end!=std::string::npos?s.substr(begin,end-begin):std::string{};
    };
    if(bindings(original).empty()||bindings(original)!=bindings(finalIr)
        ||finalIr.find("call i32 @dx.op.bufferUpdateCounter")!=std::string::npos
        ||(kind==ShaderKind::Prepass&&finalIr.find("HairPrevPositionBuffers")==std::string::npos)) {
        output.clear();error="shader interface or extension validation failed";return false;
    }
    return true;
}
bool ShaderCache::CompileConverter(std::vector<std::byte>& output,std::string& error) {
    return CompileProgram(kConverterHlsl,L"cs_6_5",output,error);
}
bool ShaderCache::CompileProgram(std::string_view source,const wchar_t* target,std::vector<std::byte>& output,std::string& error) {
    output.clear();
    if(!compiler_||source.empty()||source.size()>1024*1024||!target) {error="invalid compiler input";return false;}
    DxcBuffer input{source.data(),source.size(),CP_UTF8};
    const wchar_t* args[]={L"-E",L"main",L"-T",target,L"-O3",L"-HV",L"2021"};
    ComPtr<IDxcResult> result;ComPtr<IDxcBlob> blob;
    if(FAILED(compiler_->Compile(&input,args,_countof(args),nullptr,IID_PPV_ARGS(&result)))||!Result(result.Get(),blob,error))return false;
    return Validate(blob.Get(),output,error);
}
bool ShaderCache::Prepare(HMODULE game,const std::array<uint32_t,4>& rvas,const std::wstring& directory,std::string& error) {
    if(!game) {error="no game image";return false;}
    if(!Initialize(directory,error)||!CompileConverter(converter_,error))return false;
    // Each embedded copy must be one known shader of its kind (exact size and
    // SHA-256); both copies of a kind must be the same shader.
    for(size_t i=0;i<rvas.size();++i) {
        const auto kind=i<2?ShaderKind::ClosestHit:ShaderKind::Prepass;
        const auto* p=reinterpret_cast<const std::byte*>(game)+rvas[i];
        const ShaderIdentity* identity=nullptr;std::vector<std::byte> source;
        for(const auto& known:KnownShaders()) {
            if(known.kind!=kind||!Readable(p,known.size))continue;
            source.resize(known.size);
            if(CopyChecked(source.data(),p,source.size())&&HashEquals(source,known.sha256)) {identity=&known;break;}
        }
        auto& selected=kind==ShaderKind::ClosestHit?closestIdentity_:prepassIdentity_;
        if(i%2==0)selected=nullptr;
        if(!identity||(i%2==1&&identity!=selected)) {error="live hair shader identity mismatch";return false;}
        selected=identity;originals_[i]=p;
        if(i==0&&!Translate(source,kind,closest_,error))return false;
        if(i==2&&!Translate(source,kind,prepass_,error))return false;
    }
    return Ready();
}
std::span<const std::byte> ShaderCache::Replacement(const void* data,size_t size) const noexcept {
    // Some engines copy embedded DXIL before creating the state object. Size is
    // only a cheap filter; exact live content establishes the shader identity.
    try {
        if(!Ready()||!closestIdentity_||!prepassIdentity_||(size!=closestIdentity_->size&&size!=prepassIdentity_->size)||!Readable(data,size))return {};
        const auto* identity=size==closestIdentity_->size?closestIdentity_:prepassIdentity_;
        std::vector<std::byte> copy(size);
        if(!CopyChecked(copy.data(),data,size)||!HashEquals(copy,identity->sha256))return {};
        return identity==closestIdentity_?std::span<const std::byte>(closest_):std::span<const std::byte>(prepass_);
    } catch(...) {}
    return {};
}
}
