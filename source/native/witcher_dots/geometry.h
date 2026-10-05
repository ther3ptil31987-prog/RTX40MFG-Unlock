#pragma once
#include <array>
#include <cstdint>
#include <span>

namespace witcher_dots {
struct Vec3 { float x{}, y{}, z{}; };
struct StrandVertex { Vec3 position; float radius{}; };
constexpr uint32_t kTrianglesPerSegment = 4;
constexpr uint32_t kVerticesPerSegment = 12;
constexpr uint64_t kBytesPerSegment = 144;
constexpr uint64_t kGeometryBudget = 512ull * 1024 * 1024;
// The converted-vertex pool holds kGeometryBudget in steady play. A save load
// or area change keeps the old area's recordings in flight while the new hair
// is built: the pool may then grow to kGeometryCeiling, only while the OS
// video-memory budget keeps kVramReserve free (the game does not retry a hair
// build that was refused, so that hair would stay missing until recreated).
constexpr uint64_t kGeometryCeiling = 2048ull * 1024 * 1024;
constexpr uint64_t kVramReserve = 1024ull * 1024 * 1024;
constexpr uint32_t kMaxSegments = 65535u * 64u;
struct Plan {
    uint32_t segments{}, vertices{}, groups{};
    uint64_t bytes{};
    explicit operator bool() const noexcept { return segments != 0; }
};
// Bound DXR primitive count, dispatch dimensions and all buffer arithmetic.
Plan MakePlan(uint64_t segments, uint64_t sourceVertices, uint64_t sourceIndices,
    uint64_t budget = kGeometryBudget) noexcept;
std::array<Vec3, kVerticesPerSegment> Tessellate(StrandVertex p, StrandVertex q) noexcept;
float StrandU(uint32_t triangle, float baryX, float baryY) noexcept;
uint64_t SourceVertex(uint32_t triangle, uint32_t segmentsPerStrand) noexcept;
// Reconstruct a rounded tapered strand normal from the ray. Finite fallback
// covers parallel rays, missed cones, collapsed strands and extreme inputs.
Vec3 RoundedNormal(StrandVertex p, StrandVertex q, Vec3 origin, Vec3 direction,
    Vec3 fallback) noexcept;
}
