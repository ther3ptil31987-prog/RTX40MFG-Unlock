#pragma once
#include <cstdint>

// Rows of the generated program table (embedded_xvk_programs.generated.h, xess/vulkan/generate_xvk_programs.py).
namespace xvk::embedded
{
struct BindingRow
{
    uint16_t binding;
    uint16_t type;   // 0 storage buffer, 1 uniform buffer, 2 sampled image, 3 storage image, 4 sampler
};
struct ProgramAsset
{
    const char* dxilSha256;   // the program this SPIR-V replaces
    unsigned resource;        // RCDATA id of the SPIR-V
    const char* spirvSha256;
    uint32_t spirvBytes;
    uint32_t pushConstantBytes;
    uint32_t firstBinding;    // into bindings[]
    uint32_t bindingCount;
};
}
