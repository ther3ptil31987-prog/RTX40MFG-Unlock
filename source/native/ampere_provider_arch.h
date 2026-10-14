#pragma once
#include <Windows.h>
#include <cstddef>
#include <cstdint>

// RTX 30 support declared by the DLSS-G provider itself.
//
// Every audited DLSS-G provider (310.1.0 through 310.9.1, game, NGX OTA and
// driver copies) declares Ada as its minimum architecture in two places:
//  - the NVSDK_NGX_GetGPUArchitecture export (mov eax, 0x190; ret), which the
//    NGX core compares against the GPU for snippet metadata and again for
//    CreateFeature validation (0xBAD00001 / 0xBAD0000B on RTX 30), and
//  - NVSDK_NGX_D3D12_GetFeatureRequirements, whose request record stores the
//    FG feature id (0x0B) followed by the minimum architecture (0x190), which
//    Streamline reads as the plugin's minimum GPU architecture.
// On a system with an RTX 30 GPU both immediates are rewritten in process
// memory to 0x170, so NGX and Streamline admit Ampere natively. No NVIDIA
// file, driver or Streamline code is modified. Nothing changes on other GPUs.
namespace ampere_provider_arch
{
inline constexpr uint32_t kAdaArchitecture = 0x190;
inline constexpr uint32_t kAmpereArchitecture = 0x170;
inline constexpr uint32_t kFrameGenerationFeature = 0x0B;
inline constexpr char kArchitectureExport[] = "NVSDK_NGX_GetGPUArchitecture";
inline constexpr char kRequirementsExport[] = "NVSDK_NGX_D3D12_GetFeatureRequirements";

// mov eax, imm32; ret
constexpr bool ArchitectureExportIs(const uint8_t* code, uint32_t architecture) noexcept
{
    return code && code[0] == 0xB8
        && code[1] == static_cast<uint8_t>(architecture)
        && code[2] == static_cast<uint8_t>(architecture >> 8)
        && code[3] == 0 && code[4] == 0 && code[5] == 0xC3;
}

// NVIDIA's NV_GPU_ARCH_INFO identifies consumer/workstation Ampere RTX parts
// as architecture 0x170 with implementation GA102/103/104/106/107. GA100
// (no RT cores) and GA10B (Tegra) are excluded.
constexpr bool IsAmpereRtx(uint32_t architecture, uint32_t implementation) noexcept
{
    return architecture == kAmpereArchitecture
        && (implementation == 2 || implementation == 3 || implementation == 4
            || implementation == 6 || implementation == 7);
}

static_assert(IsAmpereRtx(0x170, 2) && IsAmpereRtx(0x170, 7));
static_assert(!IsAmpereRtx(0x170, 0) && !IsAmpereRtx(0x170, 0xB) && !IsAmpereRtx(0x190, 2));

enum class Site : uint32_t { eNone = 0, eExport = 1, eRequirements = 2 };

// Locates the unique `mov dword [rsp+d8], 0x190|0x170` in a decoded function
// body that follows `mov dword [rsp+d8-4], 0x0B`. Returns the offset of the
// architecture immediate's low byte from `begin`, or SIZE_MAX when the shape
// is absent or ambiguous. The bytes must be readable for [begin, begin+size).
size_t FindRequirementsArchitecture(const uint8_t* begin, size_t size) noexcept;

enum class Outcome : uint32_t
{
    eNotArmed = 0,
    eNotProvider = 1,
    eDeclared = 2,
    eAlreadyDeclared = 3,
    eShapeMismatch = 4,
    ePublicationFailed = 5,
};
const char* OutcomeName(Outcome outcome) noexcept;

// Detects an RTX 30 GPU through NVAPI and arms the declaration when one is
// present. Loads nvapi64.dll: never call this under the loader lock.
bool DetectAndArm() noexcept;
// Arms the declaration for an established RTX 30 GPU (IsAmpereRtx). Used by
// DetectAndArm after its NVAPI check, and directly by validation harnesses.
void Arm(uint32_t architecture, uint32_t implementation) noexcept;
bool Armed() noexcept;
bool Detected() noexcept;

// Declares Ampere support in one mapped DLSS-G provider when armed. Safe in a
// loader notification: no waits, allocation or logging. Idempotent.
Outcome Declare(HMODULE module) noexcept;

struct Snapshot
{
    bool detected = false;
    bool armed = false;
    uint32_t gpuArchitecture = 0;
    uint32_t gpuImplementation = 0;
    // Inventory evidence for pre-slInit plugin preferences only. This does
    // not select the rendering adapter or grant provider publication rights.
    uint32_t physicalGpuCount = 0;
    uint32_t singleGpuArchitecture = 0, singleGpuImplementation = 0;
    uint64_t declared = 0;
    uint64_t alreadyDeclared = 0;
    uint64_t shapeMismatches = 0;
    uint64_t publicationFailures = 0;
    Outcome last = Outcome::eNotArmed;
    HMODULE lastModule = nullptr;
};
Snapshot Read() noexcept;
}
