#pragma once

// Pure HUDless detection logic: tile layout, texel decoding, colour-relation
// statistics and classification. No graphics API calls, allocation ownership
// or game state; the D3D12 recording glue lives in hudless_probe.cpp.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>

namespace hudless_detection
{
constexpr uint32_t kTileSize = 16;
constexpr uint32_t kTileColumns = 8;
constexpr uint32_t kTileRows = 6;
constexpr uint32_t kTileCount = kTileColumns * kTileRows;
constexpr uint32_t kMaximumSamples = kTileCount * kTileSize * kTileSize;
// A probe needs enough lit pixels to distinguish colour relations; loading
// screens and fades are reported as insufficient rather than guessed.
constexpr uint32_t kMinimumSignalSamples = 1500;
constexpr uint32_t kDecisionPermille = 600;
constexpr uint32_t kMinimumInformativeTiles = 12;
constexpr float kSignalFloor = 0.02f;

struct Rect
{
    uint32_t left = 0;
    uint32_t top = 0;
    uint32_t width = 0;
    uint32_t height = 0;
};

struct TileLayout
{
    uint32_t count = 0;
    std::array<Rect, kTileCount> tiles{};
};

// Tiles are placed at grid-cell centres of a width x height region, in region
// coordinates. The same layout is applied to the HUDless extent and to the
// final colour region so both sides sample identical screen positions.
inline TileLayout Layout(uint32_t width, uint32_t height) noexcept
{
    TileLayout layout{};
    if (!width || !height) return layout;
    const uint32_t tileWidth = std::min(kTileSize, width);
    const uint32_t tileHeight = std::min(kTileSize, height);
    for (uint32_t row = 0; row < kTileRows; ++row)
    {
        for (uint32_t column = 0; column < kTileColumns; ++column)
        {
            const uint64_t centreX = (2ull * column + 1) * width / (2ull * kTileColumns);
            const uint64_t centreY = (2ull * row + 1) * height / (2ull * kTileRows);
            Rect tile{};
            tile.width = tileWidth;
            tile.height = tileHeight;
            tile.left = static_cast<uint32_t>(std::min<uint64_t>(
                centreX > tileWidth / 2 ? centreX - tileWidth / 2 : 0, width - tileWidth));
            tile.top = static_cast<uint32_t>(std::min<uint64_t>(
                centreY > tileHeight / 2 ? centreY - tileHeight / 2 : 0, height - tileHeight));
            layout.tiles[layout.count++] = tile;
        }
    }
    return layout;
}

// DXGI_FORMAT values, kept numeric so this header stays API independent.
enum Format : uint32_t
{
    eR32G32B32A32_TYPELESS = 1,
    eR32G32B32A32_FLOAT = 2,
    eR16G16B16A16_TYPELESS = 9,
    eR16G16B16A16_FLOAT = 10,
    eR16G16B16A16_UNORM = 11,
    eR10G10B10A2_TYPELESS = 23,
    eR10G10B10A2_UNORM = 24,
    eR11G11B10_FLOAT = 26,
    eR8G8B8A8_TYPELESS = 27,
    eR8G8B8A8_UNORM = 28,
    eR8G8B8A8_UNORM_SRGB = 29,
    eB8G8R8A8_UNORM = 87,
    eB8G8R8X8_UNORM = 88,
    eB8G8R8A8_TYPELESS = 90,
    eB8G8R8A8_UNORM_SRGB = 91,
    eB8G8R8X8_TYPELESS = 92,
    eB8G8R8X8_UNORM_SRGB = 93,
};

struct FormatInfo
{
    bool supported = false;
    uint32_t bytesPerPixel = 0;
    // Decoded values follow shader-view semantics: *_SRGB formats are
    // linearised, UNORM and typeless formats return stored values.
    bool srgb = false;
    bool floating = false;
};

inline FormatInfo Describe(uint32_t format) noexcept
{
    switch (format)
    {
    case eR32G32B32A32_TYPELESS:
    case eR32G32B32A32_FLOAT: return {true, 16, false, true};
    case eR16G16B16A16_TYPELESS:
    case eR16G16B16A16_FLOAT: return {true, 8, false, true};
    case eR16G16B16A16_UNORM: return {true, 8, false, false};
    case eR10G10B10A2_TYPELESS:
    case eR10G10B10A2_UNORM: return {true, 4, false, false};
    case eR11G11B10_FLOAT: return {true, 4, false, true};
    case eR8G8B8A8_TYPELESS:
    case eR8G8B8A8_UNORM:
    case eB8G8R8A8_UNORM:
    case eB8G8R8X8_UNORM:
    case eB8G8R8A8_TYPELESS:
    case eB8G8R8X8_TYPELESS: return {true, 4, false, false};
    case eR8G8B8A8_UNORM_SRGB:
    case eB8G8R8A8_UNORM_SRGB:
    case eB8G8R8X8_UNORM_SRGB: return {true, 4, true, false};
    default: return {};
    }
}

// A typed view format supplied with the tag refines a typeless resource
// format only within the same storage family.
inline uint32_t RefineFormat(uint32_t resourceFormat, uint32_t viewFormat) noexcept
{
    if (!viewFormat || viewFormat == resourceFormat) return resourceFormat;
    const auto family = [](uint32_t format) -> uint32_t {
        switch (format)
        {
        case eR32G32B32A32_TYPELESS: case eR32G32B32A32_FLOAT: return 1;
        case eR16G16B16A16_TYPELESS: case eR16G16B16A16_FLOAT:
        case eR16G16B16A16_UNORM: return 2;
        case eR10G10B10A2_TYPELESS: case eR10G10B10A2_UNORM: return 3;
        case eR8G8B8A8_TYPELESS: case eR8G8B8A8_UNORM:
        case eR8G8B8A8_UNORM_SRGB: return 4;
        case eB8G8R8A8_UNORM: case eB8G8R8A8_TYPELESS:
        case eB8G8R8A8_UNORM_SRGB: return 5;
        case eB8G8R8X8_UNORM: case eB8G8R8X8_TYPELESS:
        case eB8G8R8X8_UNORM_SRGB: return 6;
        case eR11G11B10_FLOAT: return 7;
        default: return 0;
        }
    };
    const uint32_t a = family(resourceFormat);
    return a && a == family(viewFormat) ? viewFormat : resourceFormat;
}

inline float HalfToFloat(uint16_t value) noexcept
{
    const uint32_t sign = (value >> 15) & 1u;
    const uint32_t exponent = (value >> 10) & 0x1fu;
    const uint32_t mantissa = value & 0x3ffu;
    float magnitude;
    if (exponent == 0) magnitude = std::ldexp(static_cast<float>(mantissa), -24);
    else if (exponent == 31) magnitude = mantissa ? NAN : INFINITY;
    else magnitude = std::ldexp(static_cast<float>(mantissa | 0x400u), static_cast<int>(exponent) - 25);
    return sign ? -magnitude : magnitude;
}

// Unsigned small floats used by R11G11B10_FLOAT: 5-bit exponent, bias 15.
inline float SmallFloatToFloat(uint32_t bits, uint32_t mantissaBits) noexcept
{
    const uint32_t exponent = (bits >> mantissaBits) & 0x1fu;
    const uint32_t mantissa = bits & ((1u << mantissaBits) - 1u);
    if (exponent == 0)
        return std::ldexp(static_cast<float>(mantissa), -14 - static_cast<int>(mantissaBits));
    if (exponent == 31) return mantissa ? NAN : INFINITY;
    return std::ldexp(static_cast<float>(mantissa | (1u << mantissaBits)),
        static_cast<int>(exponent) - 15 - static_cast<int>(mantissaBits));
}

inline float SrgbToLinear(float value) noexcept
{
    return value <= 0.04045f ? value / 12.92f
        : std::pow((value + 0.055f) / 1.055f, 2.4f);
}

inline float LinearToSrgb(float value) noexcept
{
    value = std::clamp(value, 0.0f, 1.0f);
    return value <= 0.0031308f ? value * 12.92f
        : 1.055f * std::pow(value, 1.0f / 2.4f) - 0.055f;
}

inline float LinearToPq(float nits) noexcept
{
    constexpr float m1 = 2610.0f / 16384.0f;
    constexpr float m2 = 2523.0f / 4096.0f * 128.0f;
    constexpr float c1 = 3424.0f / 4096.0f;
    constexpr float c2 = 2413.0f / 4096.0f * 32.0f;
    constexpr float c3 = 2392.0f / 4096.0f * 32.0f;
    const float y = std::pow(std::clamp(nits / 10000.0f, 0.0f, 1.0f), m1);
    return std::pow((c1 + c2 * y) / (1.0f + c3 * y), m2);
}

inline uint32_t Read32(const uint8_t* bytes) noexcept
{
    uint32_t value;
    std::memcpy(&value, bytes, sizeof(value));
    return value;
}

inline uint16_t Read16(const uint8_t* bytes) noexcept
{
    uint16_t value;
    std::memcpy(&value, bytes, sizeof(value));
    return value;
}

// Decodes RGB of one texel. Returns false for unsupported formats.
inline bool Decode(uint32_t format, const uint8_t* texel, float rgb[3]) noexcept
{
    const FormatInfo info = Describe(format);
    if (!info.supported || !texel) return false;
    switch (format)
    {
    case eR32G32B32A32_TYPELESS:
    case eR32G32B32A32_FLOAT:
        for (int c = 0; c < 3; ++c) std::memcpy(&rgb[c], texel + 4 * c, 4);
        return true;
    case eR16G16B16A16_TYPELESS:
    case eR16G16B16A16_FLOAT:
        for (int c = 0; c < 3; ++c) rgb[c] = HalfToFloat(Read16(texel + 2 * c));
        return true;
    case eR16G16B16A16_UNORM:
        for (int c = 0; c < 3; ++c) rgb[c] = Read16(texel + 2 * c) / 65535.0f;
        return true;
    case eR10G10B10A2_TYPELESS:
    case eR10G10B10A2_UNORM:
    {
        const uint32_t v = Read32(texel);
        for (int c = 0; c < 3; ++c) rgb[c] = ((v >> (10 * c)) & 0x3ffu) / 1023.0f;
        return true;
    }
    case eR11G11B10_FLOAT:
    {
        const uint32_t v = Read32(texel);
        rgb[0] = SmallFloatToFloat(v & 0x7ffu, 6);
        rgb[1] = SmallFloatToFloat((v >> 11) & 0x7ffu, 6);
        rgb[2] = SmallFloatToFloat((v >> 22) & 0x3ffu, 5);
        return true;
    }
    default:
        break;
    }
    const bool bgra = format >= eB8G8R8A8_UNORM;
    for (int c = 0; c < 3; ++c)
    {
        const int source = bgra ? 2 - c : c;
        float value = texel[source] / 255.0f;
        if (info.srgb) value = SrgbToLinear(value);
        rgb[c] = value;
    }
    return true;
}

// Representable step at a value, used to scale comparison tolerances.
inline float Quantum(uint32_t format, float value) noexcept
{
    const float magnitude = std::fabs(value);
    switch (format)
    {
    case eR32G32B32A32_TYPELESS:
    case eR32G32B32A32_FLOAT: return magnitude * 1.0e-6f + 1.0e-7f;
    case eR16G16B16A16_TYPELESS:
    case eR16G16B16A16_FLOAT: return std::max(magnitude * 0.0009765625f, 0.00006103515625f);
    case eR11G11B10_FLOAT: return std::max(magnitude * 0.03125f, 0.00006103515625f);
    case eR16G16B16A16_UNORM: return 1.0f / 65535.0f;
    case eR10G10B10A2_TYPELESS:
    case eR10G10B10A2_UNORM: return 1.0f / 1023.0f;
    default:
        if (Describe(format).srgb)
        {
            // An 8-bit sRGB step expressed in linear units near this value.
            const float encoded = LinearToSrgb(value);
            return std::max(std::fabs(SrgbToLinear(std::min(encoded + 1.0f / 255.0f, 1.0f)) - value),
                1.0f / (255.0f * 12.92f));
        }
        return 1.0f / 255.0f;
    }
}

enum class Hypothesis : uint32_t
{
    eIdentity = 0,     // final == HUDless
    eSrgbEncode,       // final == sRGB OETF(HUDless)
    eGamma22Encode,    // final == HUDless^(1/2.2)
    eSrgbDecode,       // final == sRGB EOTF(HUDless)
    ePqFromScrgb,      // final == PQ(BT.2020(HUDless) * 80 nits)
    eCount,
};
constexpr size_t kHypothesisCount = static_cast<size_t>(Hypothesis::eCount);

inline const char* HypothesisName(Hypothesis hypothesis) noexcept
{
    switch (hypothesis)
    {
    case Hypothesis::eIdentity: return "identity";
    case Hypothesis::eSrgbEncode: return "srgb-encode";
    case Hypothesis::eGamma22Encode: return "gamma22-encode";
    case Hypothesis::eSrgbDecode: return "srgb-decode";
    case Hypothesis::ePqFromScrgb: return "pq-from-scrgb";
    default: return "none";
    }
}

struct Statistics
{
    uint32_t samples = 0;
    uint32_t signalSamples = 0;
    std::array<uint32_t, kHypothesisCount> matches{};
    // Sum over lit samples of the largest channel error; breaks near-ties
    // between similar curves such as sRGB and gamma 2.2.
    std::array<double, kHypothesisCount> error{};
    // Tile-level compositing evidence (see AccumulateTile).
    uint32_t tiles = 0;
    uint32_t informativeTiles = 0;
    uint32_t identityTiles = 0;
    uint32_t translucentTiles = 0;
};

inline bool Finite(const float rgb[3]) noexcept
{
    return std::isfinite(rgb[0]) && std::isfinite(rgb[1]) && std::isfinite(rgb[2]);
}

inline void Transform(Hypothesis hypothesis, const float input[3], float output[3]) noexcept
{
    switch (hypothesis)
    {
    case Hypothesis::eSrgbEncode:
        for (int c = 0; c < 3; ++c) output[c] = LinearToSrgb(input[c]);
        return;
    case Hypothesis::eGamma22Encode:
        for (int c = 0; c < 3; ++c)
            output[c] = std::pow(std::clamp(input[c], 0.0f, 1.0f), 1.0f / 2.2f);
        return;
    case Hypothesis::eSrgbDecode:
        for (int c = 0; c < 3; ++c) output[c] = SrgbToLinear(std::clamp(input[c], 0.0f, 1.0f));
        return;
    case Hypothesis::ePqFromScrgb:
    {
        // BT.709 -> BT.2020 primaries; scRGB 1.0 corresponds to 80 nits.
        const float r = 0.6274040f * input[0] + 0.3292820f * input[1] + 0.0433136f * input[2];
        const float g = 0.0690970f * input[0] + 0.9195400f * input[1] + 0.0113612f * input[2];
        const float b = 0.0163916f * input[0] + 0.0880132f * input[1] + 0.8955950f * input[2];
        output[0] = LinearToPq(r * 80.0f);
        output[1] = LinearToPq(g * 80.0f);
        output[2] = LinearToPq(b * 80.0f);
        return;
    }
    default:
        for (int c = 0; c < 3; ++c) output[c] = input[c];
        return;
    }
}

// Accumulates one co-located sample. Identity uses the storage precision of
// both formats; converted relations allow for small transfer-function
// approximations in game shaders.
inline void Accumulate(Statistics& statistics, uint32_t hudlessFormat,
    const float hudless[3], uint32_t finalFormat, const float final[3]) noexcept
{
    ++statistics.samples;
    if (!Finite(hudless) || !Finite(final)) return;
    if (std::max({final[0], final[1], final[2]}) < kSignalFloor) return;
    ++statistics.signalSamples;
    for (size_t index = 0; index < kHypothesisCount; ++index)
    {
        const auto hypothesis = static_cast<Hypothesis>(index);
        float expected[3];
        Transform(hypothesis, hudless, expected);
        bool match = true;
        float worst = 0.0f;
        for (int c = 0; c < 3; ++c)
        {
            const float difference = std::fabs(expected[c] - final[c]);
            const float tolerance = hypothesis == Hypothesis::eIdentity
                ? 1.5f * (Quantum(finalFormat, final[c]) + Quantum(hudlessFormat, hudless[c])) + 1.0e-6f
                : 1.5f * Quantum(finalFormat, final[c]) + 0.006f
                    + (Describe(finalFormat).floating ? 0.01f * std::fabs(final[c]) : 0.0f);
            match = match && difference <= tolerance;
            worst = std::max(worst, std::isfinite(difference) ? difference : 1.0f);
        }
        statistics.error[index] += worst;
        if (match) ++statistics.matches[index];
    }
}

// Tile-level evidence under Streamline's compositing contract,
// final = UI + (1 - alpha) * HUDless with premultiplied UI. A pixel-exact
// comparison fails wherever UI covers the scene, including full-screen
// translucent dimming; within a tile covered by uniform translucent UI the
// final colour is instead an exact affine function of HUDless with one slope
// (1 - alpha) shared by all channels and offsets bounded by alpha. The
// premultiplied bound (offset <= 1 - slope) rejects transfer-function
// mismatches, whose local tangents violate it. Applies to UNORM final colour;
// scRGB UI is unbounded, so floating-point finals use identity evidence only.
inline void AccumulateTile(Statistics& statistics, uint32_t hudlessFormat,
    const float* hudless, uint32_t finalFormat, const float* final, uint32_t count) noexcept
{
    ++statistics.tiles;
    if (!count) return;
    uint32_t signal = 0;
    uint32_t exact = 0;
    double meanH[3]{}, meanF[3]{};
    for (uint32_t i = 0; i < count; ++i)
    {
        const float* h = hudless + 3 * i;
        const float* f = final + 3 * i;
        if (!Finite(h) || !Finite(f)) return;
        if (std::max({f[0], f[1], f[2]}) >= kSignalFloor) ++signal;
        bool same = true;
        for (int c = 0; c < 3; ++c)
        {
            same = same && std::fabs(h[c] - f[c])
                <= 1.5f * (Quantum(finalFormat, f[c]) + Quantum(hudlessFormat, h[c])) + 1.0e-6f;
            meanH[c] += h[c];
            meanF[c] += f[c];
        }
        if (same) ++exact;
    }
    double varH[3]{}, varF[3]{}, cov[3]{};
    for (int c = 0; c < 3; ++c) { meanH[c] /= count; meanF[c] /= count; }
    for (uint32_t i = 0; i < count; ++i)
        for (int c = 0; c < 3; ++c)
        {
            const double dh = hudless[3 * i + c] - meanH[c];
            const double df = final[3 * i + c] - meanF[c];
            varH[c] += dh * dh;
            varF[c] += df * df;
            cov[c] += dh * df;
        }
    double maxStdH = 0.0, maxStdF = 0.0;
    for (int c = 0; c < 3; ++c)
    {
        maxStdH = std::max(maxStdH, std::sqrt(varH[c] / count));
        maxStdF = std::max(maxStdF, std::sqrt(varF[c] / count));
    }
    // Flat scene or flat (opaque UI) tiles carry no evidence either way.
    if (signal * 2 < count || maxStdH < 0.01 || maxStdF < 0.005) return;
    ++statistics.informativeTiles;
    if (exact * 10 >= count * 9)
    {
        ++statistics.identityTiles;
        return;
    }
    if (Describe(finalFormat).floating) return;

    const double step = Quantum(finalFormat, 0.5f);
    double slopes[3]{};
    int fitted = 0;
    double slopeSum = 0.0;
    for (int c = 0; c < 3; ++c)
    {
        if (varH[c] / count < 1.0e-4) continue; // Channel too flat to fit.
        const double slope = cov[c] / varH[c];
        const double offset = meanF[c] - slope * meanH[c];
        if (slope < 0.05 || slope > 1.02 || offset < -0.01 || offset > 1.0 - slope + 0.01) return;
        double worst = 0.0;
        for (uint32_t i = 0; i < count; ++i)
            worst = std::max(worst, std::fabs(final[3 * i + c] - (slope * hudless[3 * i + c] + offset)));
        if (worst > 3.0 * step + 0.004) return;
        slopes[fitted++] = slope;
        slopeSum += slope;
    }
    if (fitted < 2) return;
    const double mean = slopeSum / fitted;
    for (int i = 0; i < fitted; ++i)
        if (std::fabs(slopes[i] - mean) > 0.03) return;
    ++statistics.translucentTiles;
}

enum class Outcome : uint32_t
{
    eNone = 0,
    eEquivalent,
    eEncodingDiffers,
    eContentDiffers,
    eInsufficientSignal,
};

inline const char* OutcomeName(Outcome outcome) noexcept
{
    switch (outcome)
    {
    case Outcome::eEquivalent: return "equivalent";
    case Outcome::eEncodingDiffers: return "encoding-differs";
    case Outcome::eContentDiffers: return "content-differs";
    case Outcome::eInsufficientSignal: return "insufficient-signal";
    default: return "none";
    }
}

struct ProbeResult
{
    Outcome outcome = Outcome::eNone;
    uint32_t samples = 0;
    uint32_t signalSamples = 0;
    uint32_t identityPermille = 0;
    Hypothesis bestHypothesis = Hypothesis::eIdentity;
    uint32_t bestPermille = 0;
    // Fraction of lit samples that differ from HUDless. Meaningful only when
    // the relation is identity; approximates on-screen UI coverage.
    uint32_t uiCoveragePermille = 0;
    // Informative tiles explained by identity or uniform translucent UI.
    uint32_t informativeTiles = 0;
    uint32_t translucentTiles = 0;
    uint32_t compositePermille = 0;
};

inline ProbeResult Evaluate(const Statistics& statistics) noexcept
{
    ProbeResult result{};
    result.samples = statistics.samples;
    result.signalSamples = statistics.signalSamples;
    if (statistics.signalSamples < kMinimumSignalSamples)
    {
        result.outcome = Outcome::eInsufficientSignal;
        return result;
    }
    const auto permille = [&](size_t index) {
        return static_cast<uint32_t>(uint64_t(statistics.matches[index]) * 1000
            / statistics.signalSamples);
    };
    result.identityPermille = permille(0);
    result.bestHypothesis = Hypothesis::eIdentity;
    result.bestPermille = result.identityPermille;
    constexpr uint32_t kTieWindowPermille = 5;
    for (size_t index = 1; index < kHypothesisCount; ++index)
    {
        const uint32_t value = permille(index);
        const size_t current = static_cast<size_t>(result.bestHypothesis);
        const bool nearTie = value + kTieWindowPermille >= result.bestPermille
            && result.bestPermille + kTieWindowPermille >= value;
        if ((nearTie && statistics.error[index] < statistics.error[current])
            || (!nearTie && value > result.bestPermille))
        {
            result.bestPermille = value;
            result.bestHypothesis = static_cast<Hypothesis>(index);
        }
    }
    result.informativeTiles = statistics.informativeTiles;
    result.translucentTiles = statistics.translucentTiles;
    if (statistics.informativeTiles)
        result.compositePermille = (statistics.identityTiles + statistics.translucentTiles) * 1000
            / statistics.informativeTiles;
    const bool composited = statistics.informativeTiles >= kMinimumInformativeTiles
        && result.compositePermille >= kDecisionPermille;
    if (result.identityPermille >= kDecisionPermille || composited)
    {
        result.outcome = Outcome::eEquivalent;
        result.bestHypothesis = Hypothesis::eIdentity;
        result.bestPermille = std::max(result.identityPermille, result.compositePermille);
        result.uiCoveragePermille = 1000 - result.identityPermille;
    }
    else if (result.bestPermille >= kDecisionPermille)
        result.outcome = Outcome::eEncodingDiffers;
    else
        result.outcome = Outcome::eContentDiffers;
    return result;
}

// Chooses the better of two final-colour captures (the Present that followed
// the HUDless copy and the one after it), absorbing one frame of pipelining.
inline ProbeResult Better(const ProbeResult& a, const ProbeResult& b, uint32_t& chosen) noexcept
{
    const auto rank = [](const ProbeResult& r) {
        const uint32_t conclusive = r.outcome == Outcome::eEquivalent ? 3
            : r.outcome == Outcome::eEncodingDiffers ? 2
            : r.outcome == Outcome::eContentDiffers ? 1 : 0;
        return (uint64_t(conclusive) << 32) | r.bestPermille;
    };
    chosen = rank(b) > rank(a) ? 1u : 0u;
    return chosen ? b : a;
}

enum class Verdict : uint32_t
{
    eNotMeasured = 0,
    eMeasuring,
    eEquivalent,
    eEncodingDiffers,
    eContentDiffers,
    eShapeMismatch,
    eUnsupportedFormat,
    eUnavailable,
};

inline const char* VerdictName(Verdict verdict) noexcept
{
    switch (verdict)
    {
    case Verdict::eMeasuring: return "measuring";
    case Verdict::eEquivalent: return "equivalent";
    case Verdict::eEncodingDiffers: return "encoding-differs";
    case Verdict::eContentDiffers: return "content-differs";
    case Verdict::eShapeMismatch: return "shape-mismatch";
    case Verdict::eUnsupportedFormat: return "unsupported-format";
    case Verdict::eUnavailable: return "unavailable";
    default: return "not-measured";
    }
}

// Keeps the latest conclusive probe outcomes. A verdict needs at least three
// agreeing probes and a majority of the retained window, so menus or a single
// transition cannot decide it.
class Aggregator
{
public:
    static constexpr size_t kWindow = 8;
    static constexpr uint32_t kRequiredAgreement = 3;

    void Add(const ProbeResult& result) noexcept
    {
        ++probes_;
        if (result.outcome == Outcome::eNone || result.outcome == Outcome::eInsufficientSignal)
            return;
        ++conclusive_;
        window_[next_] = result.outcome;
        next_ = (next_ + 1) % kWindow;
        filled_ = std::min(filled_ + 1, kWindow);
    }

    void Reset() noexcept { *this = {}; }

    uint32_t Probes() const noexcept { return probes_; }
    uint32_t Conclusive() const noexcept { return conclusive_; }

    Verdict Current() const noexcept
    {
        if (!probes_) return Verdict::eNotMeasured;
        std::array<uint32_t, 5> counts{};
        for (size_t i = 0; i < filled_; ++i) ++counts[static_cast<size_t>(window_[i])];
        const Outcome candidates[] = {Outcome::eEquivalent, Outcome::eEncodingDiffers,
            Outcome::eContentDiffers};
        for (const Outcome outcome : candidates)
        {
            const uint32_t count = counts[static_cast<size_t>(outcome)];
            if (count >= kRequiredAgreement && count * 2 > filled_)
            {
                return outcome == Outcome::eEquivalent ? Verdict::eEquivalent
                    : outcome == Outcome::eEncodingDiffers ? Verdict::eEncodingDiffers
                    : Verdict::eContentDiffers;
            }
        }
        return Verdict::eMeasuring;
    }

private:
    std::array<Outcome, kWindow> window_{};
    size_t next_ = 0;
    size_t filled_ = 0;
    uint32_t probes_ = 0;
    uint32_t conclusive_ = 0;
};

enum class Source : uint32_t
{
    eWaiting = 0,       // No Streamline tags observed yet
    eNoHudlessTag,      // Frame-generation inputs tagged, never a HUDless buffer
    eGameHudless,       // HUDless tagged, no UI buffer
    eGameHudlessAndUi,  // HUDless and UI alpha or colour/alpha tagged
    eGameUiOnly,        // UI buffer tagged; HUDless absent or tagged empty
};

inline const char* SourceName(Source source) noexcept
{
    switch (source)
    {
    case Source::eNoHudlessTag: return "no-hudless-tag";
    case Source::eGameHudless: return "game-hudless";
    case Source::eGameHudlessAndUi: return "game-hudless-and-ui";
    case Source::eGameUiOnly: return "game-ui-only";
    default: return "waiting";
    }
}

// Which v1.4 HUD route the detected inputs lead to. Only an identity relation
// allows the UI buffer to be derived from final colour minus HUDless.
enum class Route : uint32_t
{
    eUndetermined = 0,
    eGamePair,          // Game supplies HUDless and UI; enable recomposition
    eSynthesizeUi,      // HUDless matches final colour; derive the UI buffer
    eHudlessUnusable,   // HUDless tagged but not colour-equivalent or shaped
    eCaptureRequired,   // No HUDless tag; needs pre-UI capture
    // UI colour/alpha tagged without HUDless: under the premultiplied contract
    // HUDless = (final - UI) / (1 - alpha) wherever the UI is not opaque.
    eDeriveHudless,
};

inline const char* RouteName(Route route) noexcept
{
    switch (route)
    {
    case Route::eGamePair: return "game-pair";
    case Route::eSynthesizeUi: return "synthesize-ui";
    case Route::eHudlessUnusable: return "hudless-unusable";
    case Route::eCaptureRequired: return "capture-required";
    case Route::eDeriveHudless: return "derive-hudless";
    default: return "undetermined";
    }
}

inline Route Classify(Source source, Verdict verdict) noexcept
{
    switch (source)
    {
    case Source::eNoHudlessTag: return Route::eCaptureRequired;
    case Source::eGameUiOnly: return Route::eDeriveHudless;
    case Source::eGameHudlessAndUi:
        return verdict == Verdict::eEquivalent || verdict == Verdict::eNotMeasured
                || verdict == Verdict::eMeasuring || verdict == Verdict::eUnavailable
            ? Route::eGamePair : Route::eHudlessUnusable;
    case Source::eGameHudless:
        if (verdict == Verdict::eEquivalent) return Route::eSynthesizeUi;
        if (verdict == Verdict::eEncodingDiffers || verdict == Verdict::eContentDiffers
            || verdict == Verdict::eShapeMismatch || verdict == Verdict::eUnsupportedFormat)
            return Route::eHudlessUnusable;
        return Route::eUndetermined;
    default:
        return Route::eUndetermined;
    }
}
}
