#pragma once
#include <Windows.h>
#include <objbase.h>
#include <dxcapi.h>
#include <wrl/client.h>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>
namespace witcher_dots {
enum class ShaderKind { ClosestHit, Prepass };
// Value names (named form: %rN, see TranslateIr) the translation edits in one
// build of the game's hair shaders. ClosestHitHair: LSS hit attributes and the
// NVIDIA-extension endpoint block; PTHairPrepassRGS: the ray query's LSS hit
// test, the two strand-u sequences and the endpoint block. Both: the object-
// space normal (three values) and the instruction the rounded normal precedes.
struct ClosestAnchors {
    const char *prim,*attrLoad,*attrU,*endpointsFrom,*endpointsTo,*index;
    std::array<int,8> extracts; std::array<int,6> ray; // ray: origin xyz, direction xyz
    const char *nonuniform,*noalias;
    std::array<int,3> normals; const char* normalMarker;
};
struct PrepassAnchors {
    const char *query,*candidateType,*hitFrom,*hitBranch,*alphaPrim,*alphaUFrom,*alphaUTo,*alphaU;
    const char *mainUFrom,*mainUTo,*mainPrim,*mainU,*instance,*index,*endpointsFrom,*endpointsTo;
    std::array<int,8> extracts; std::array<int,6> ray;
    const char *nonuniform,*noalias;
    std::array<int,3> normals; const char* normalMarker;
};
// A known game hair shader: exact kind, size and SHA-256, and its anchors.
struct ShaderIdentity { ShaderKind kind; uint32_t size; const char* sha256; const ClosestAnchors* closest; const PrepassAnchors* prepass; };
std::span<const ShaderIdentity> KnownShaders();
const ShaderIdentity* FindShader(std::span<const std::byte> bytes, ShaderKind kind);
bool TranslateIr(std::string_view original, const ShaderIdentity& shader, std::string& translated, std::string& error);
std::array<uint8_t,32> Sha256(std::span<const std::byte> bytes);
bool HashEquals(std::span<const std::byte> bytes, const char* expected);
class ShaderCache {
public:
    // Explicit game-side DXC; never distribute or overwrite its compiler.
    bool Initialize(const std::wstring& directory, std::string& error);
    bool Translate(std::span<const std::byte> source, ShaderKind kind, std::vector<std::byte>& output, std::string& error);
    bool CompileConverter(std::vector<std::byte>& output, std::string& error);
    bool CompileProgram(std::string_view source, const wchar_t* target, std::vector<std::byte>& output, std::string& error);
    // `rvas`: the build's ClosestHitHair copies, then its PTHairPrepassRGS copies.
    bool Prepare(HMODULE game, const std::array<uint32_t,4>& rvas, const std::wstring& directory, std::string& error);
    std::span<const std::byte> Replacement(const void* data, size_t size) const noexcept;
    ShaderKind ReplacementKind(std::span<const std::byte> replacement) const noexcept {
        return replacement.data()==closest_.data()?ShaderKind::ClosestHit:ShaderKind::Prepass;
    }
    const std::vector<std::byte>& Converter() const noexcept { return converter_; }
    bool Ready() const noexcept { return !closest_.empty()&&!prepass_.empty()&&!converter_.empty(); }
private:
    bool Disassemble(std::span<const std::byte> source, std::string& text, std::string& error);
    bool Validate(IDxcBlob* blob, std::vector<std::byte>& output, std::string& error);
    HMODULE compilerModule_{}, validatorModule_{};
    Microsoft::WRL::ComPtr<IDxcCompiler3> compiler_;
    Microsoft::WRL::ComPtr<IDxcLibrary> library_;
    Microsoft::WRL::ComPtr<IDxcAssembler> assembler_;
    Microsoft::WRL::ComPtr<IDxcOptimizer> optimizer_;
    Microsoft::WRL::ComPtr<IDxcValidator> validator_;
    std::array<const void*,4> originals_{};
    const ShaderIdentity *closestIdentity_{},*prepassIdentity_{};
    std::vector<std::byte> closest_,prepass_,converter_;
};
}
