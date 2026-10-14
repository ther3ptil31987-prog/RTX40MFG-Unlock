#pragma once
#include <cstdint>

// RTX 30: keep the DLSS-G Streamline wrapper off flip-metering presentation.
//
// The 2.14 wrapper probes NVIDIA flip metering through nvapi_QueryInterface
// (flipMetering.cpp initialize): SetFlipConfig and its V2 capability query.
// A consumer RTX 30 driver accepts SetFlipConfig; after 30 "good" feedback
// states the wrapper presents through flip metering, which sizes its
// clone.dlfg-output pool to two surfaces instead of count + 1. Every generated
// frame of a batch then shares one output, which the Ampere backend refuses.
// With these two interfaces unavailable the wrapper keeps the CPU-paced path
// (distinct outputs; the path the A10G GRID driver takes, final-present
// validated at 4x/6x). Every other NVAPI interface is forwarded unchanged.
namespace ampere_flip_metering
{
inline constexpr uint32_t kSetFlipConfig = 0xF3148C42u;
inline constexpr uint32_t kFlipConfigCaps = 0x6194B19Du;

constexpr bool Hidden(uint32_t id) noexcept
{
    return id == kSetFlipConfig || id == kFlipConfigCaps;
}

static_assert(Hidden(kSetFlipConfig) && Hidden(kFlipConfigCaps));
static_assert(!Hidden(0x0150E828u) && !Hidden(0x85A6C2A0u)); // Initialize, GetRaytracingCaps
}
