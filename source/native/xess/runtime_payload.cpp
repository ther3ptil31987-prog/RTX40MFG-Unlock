#include "runtime_payload.h"
#include "embedded_xess_programs.generated.h"
#include "embedded_xess_shaders.generated.h"
#include <bcrypt.h>
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <sstream>
#include <stdexcept>

namespace xess_payload {
std::string Hash(std::span<const unsigned char> bytes) {
    BCRYPT_ALG_HANDLE algorithm{}; BCRYPT_HASH_HANDLE hash{};
    if(BCryptOpenAlgorithmProvider(&algorithm,BCRYPT_SHA256_ALGORITHM,nullptr,0)<0)throw std::runtime_error("hash_algorithm");
    std::array<UCHAR,32> digest{};
    NTSTATUS status=BCryptCreateHash(algorithm,&hash,nullptr,0,nullptr,0,0);
    if(status>=0)status=BCryptHashData(hash,const_cast<PUCHAR>(bytes.data()),static_cast<ULONG>(bytes.size()),0);
    if(status>=0)status=BCryptFinishHash(hash,digest.data(),static_cast<ULONG>(digest.size()),0);
    if(hash)BCryptDestroyHash(hash);
    BCryptCloseAlgorithmProvider(algorithm,0);
    if(status<0)throw std::runtime_error("hash_failed");
    std::string out;
    for(UCHAR b:digest){char h[3];sprintf_s(h,"%02x",b);out+=h;}
    return out;
}
std::span<const Asset> Assets() noexcept { return embedded::assets; }
std::span<const unsigned char> Read(HMODULE module,const Asset& asset) {
    if(!module)throw std::runtime_error("payload_module_missing");
    const auto resource=FindResourceW(module,MAKEINTRESOURCEW(asset.id),RT_RCDATA);
    const auto loaded=resource?LoadResource(module,resource):nullptr;
    const auto size=resource?SizeofResource(module,resource):0;
    const auto* bytes=loaded?static_cast<const unsigned char*>(LockResource(loaded)):nullptr;
    if(!bytes||size!=asset.bytes)throw std::runtime_error("embedded_payload_shape");
    const std::span<const unsigned char> data(bytes,size);
    if(Hash(data)!=asset.hash)throw std::runtime_error("embedded_payload_hash");
    return data;
}
Programs LoadPrograms(HMODULE module) {
    const auto all=Assets();
    const auto manifest=std::find_if(all.begin(),all.end(),[](const auto& a){return !strcmp(a.name,"programs/programs.tsv");});
    if(manifest==all.end())throw std::runtime_error("embedded_manifest_missing");
    const auto bytes=Read(module,*manifest);
    std::istringstream input(std::string(reinterpret_cast<const char*>(bytes.data()),bytes.size()));
    Programs result;std::string line;
    while(std::getline(input,line)){
        if(!line.empty()&&line.back()=='\r')line.pop_back();
        std::istringstream row(line);std::string key,hash,extra;
        if(!(row>>key>>hash)||(row>>extra)||key.size()<4||key.size()>24||hash.size()!=64
            ||!std::all_of(key.begin(),key.end(),[](char c){return c>='0'&&c<='9';}))throw std::runtime_error("embedded_manifest_row");
        const auto name="programs/"+key+".dxil";
        const auto found=std::find_if(all.begin(),all.end(),[&](const auto& a){return name==a.name;});
        if(found==all.end()||hash!=found->hash)throw std::runtime_error("embedded_program_manifest");
        const auto data=Read(module,*found);uint32_t size{};
        if(data.size()>=28)memcpy(&size,data.data()+24,4);
        if(data.size()<32||data.size()>1000000||memcmp(data.data(),"DXBC",4)||size!=data.size()
            ||!result.emplace(key,std::vector<unsigned char>(data.begin(),data.end())).second)throw std::runtime_error("embedded_program_shape");
    }
    if(result.size()!=54)throw std::runtime_error("embedded_program_count");
    return result;
}
std::span<const Asset> ShaderAssets() noexcept { return embedded::shaders; }
std::vector<Replacement> LoadReplacements(HMODULE module) {
    // The Intel program each shader replaces: XeFG's motion-vector splat in libxess_fg 3.0.2 (ec5e0c65), and
    // (splat.2) its inverted-depth variant, which XeFG uses when started with INVERTED_DEPTH (OptiScaler's default).
    struct Native { const char* name; const char* hash; size_t bytes; };
    static constexpr Native natives[]{
        {"shaders/splat_motion_aware.dxil","3f3f91d2ecd4345042f0519f7455776314be9e7730453a3fcb42c57f0acb49b5",4008},
        {"shaders/splat_motion_aware_inverted.dxil","c3649191906bdd300462dbafe6add8f82b09e8e10c41e92784a209d99513224b",4012}};
    const auto all=ShaderAssets();
    if(all.size()!=std::size(natives))throw std::runtime_error("embedded_replacement_set");
    std::vector<Replacement> result;
    for(const auto& native:natives){
        const auto found=std::find_if(all.begin(),all.end(),[&](const auto& a){return !strcmp(a.name,native.name);});
        if(found==all.end())throw std::runtime_error("embedded_replacement_set");
        const auto data=Read(module,*found);uint32_t size{};
        if(data.size()>=28)memcpy(&size,data.data()+24,4);
        if(data.size()<32||data.size()>1000000||memcmp(data.data(),"DXBC",4)||size!=data.size())
            throw std::runtime_error("embedded_replacement_shape");
        result.push_back({native.name,native.hash,native.bytes,std::vector<unsigned char>(data.begin(),data.end())});
    }
    return result;
}
}
