#pragma once

#include "dlss5_policy.h"
#include "ui_status_json.h"

#include <charconv>
#include <cmath>
#include <cstdio>
#include <string>
#include <string_view>

// The dlss5* keys of the native control file and the status protocol. The
// backend (patcher.cpp), the menu (reshade_frontend.cpp) and the status
// writer share this table, so a key, its range and its default exist once.
namespace dlss5::settings_json
{
enum class Kind : uint8_t { eBool, eUnsigned, eFloat };

struct Field
{
    const char* key;
    Kind kind;
    float minimum, maximum;
    bool Settings::* flag;
    uint32_t Settings::* integer;
    float Settings::* real;
};

inline constexpr Field Flag(const char* key, bool Settings::* member) { return {key, Kind::eBool, 0, 1, member, nullptr, nullptr}; }
inline constexpr Field Choice(const char* key, uint32_t minimum, uint32_t maximum, uint32_t Settings::* member)
{ return {key, Kind::eUnsigned, static_cast<float>(minimum), static_cast<float>(maximum), nullptr, member, nullptr}; }
inline constexpr Field Real(const char* key, float minimum, float maximum, float Settings::* member)
{ return {key, Kind::eFloat, minimum, maximum, nullptr, nullptr, member}; }

inline constexpr Field kFields[] = {
    Flag("dlss5", &Settings::enabled),
    Choice("dlss5HookStage", 0, 3, &Settings::stage),
    Choice("dlss5UseDlssResources", 0, 2, &Settings::resources),
    Choice("dlss5UiCorrection", 0, 2, &Settings::uiCorrection),
    Choice("dlss5Encoding", 0, 5, &Settings::encoding),
    Real("dlss5DiffuseWhite", kMinDiffuseWhite, kMaxDiffuseWhite, &Settings::diffuseWhite),
    Flag("dlss5DiffuseWhiteOverride", &Settings::diffuseWhiteOverride),
    Real("dlss5OverallIntensity", 0.0f, 1.0f, &Settings::intensity),
    Choice("dlss5Model", 0, 2, &Settings::model),
    Real("dlss5StructureIntensity", 0.0f, kMaxStrength, &Settings::structure),
    Real("dlss5ToneIntensity", 0.0f, kMaxStrength, &Settings::tone),
    Flag("dlss5CharacterMask", &Settings::characterMask),
    Real("dlss5CharacterIntensity", kMinCharacter, kMaxStrength, &Settings::character),
    Real("dlss5ResolutionScale", 0.0f, kMaxScale, &Settings::scale),
    Flag("dlss5ResolutionScaleAuto", &Settings::scaleAuto),
    Choice("dlss5ScalingDomain", 0, 2, &Settings::domain),
    Choice("dlss5DownsampleFilter", 0, 2, &Settings::downsample),
    Choice("dlss5UpsampleFilter", 0, 3, &Settings::upsample),
    Choice("dlss5ScaleReconstruction", 0, 2, &Settings::reconstruction),
    Choice("dlss5Preset", 0, 2, &Settings::preset),
    Choice("dlss5PassCount", 1, kMaxPasses, &Settings::passes),
    Choice("dlss5SignalEncoding", 0, 2, &Settings::signal),
    Flag("dlss5BlockHudless", &Settings::blockHudless),
    Real("dlss5Exposure", 0.0f, kMaxExposure, &Settings::exposure),
    Choice("dlss5UseExposureValue", 0, 2, &Settings::exposureMode),
    Flag("dlss5AdaptiveExposure", &Settings::adaptiveExposure),
    Flag("dlss5Boost", &Settings::boost),
    Flag("dlss5CompareSplitView", &Settings::compare),
    Real("dlss5CompareSplit", kMinCompareSplit, kMaxCompareSplit, &Settings::compareSplit),
    Choice("dlss5CompareKey", 0, 254, &Settings::compareKey),
    // dlss5adv.1: the Advanced additions.
    Flag("dlss5LaterPassesCustom", &Settings::laterCustom),
    Choice("dlss5LaterModel", 0, 2, &Settings::laterModel),
    Real("dlss5LaterIntensity", 0.0f, 1.0f, &Settings::laterIntensity),
    Real("dlss5LaterStructure", 0.0f, kMaxStrength, &Settings::laterStructure),
    Real("dlss5LaterTone", 0.0f, kMaxStrength, &Settings::laterTone),
    Real("dlss5LaterCharacter", kMinCharacter, kMaxStrength, &Settings::laterCharacter),
    Real("dlss5BlendStrength", 0.0f, kMaxBlend, &Settings::blend),
    Real("dlss5ColorStrength", 0.0f, kMaxColor, &Settings::color),
    Flag("dlss5HighlightGuard", &Settings::highlightGuard),
    Real("dlss5HighlightLimit", kMinHighlightLimit, kMaxHighlightLimit, &Settings::highlightLimit),
    Real("dlss5HaloDark", 0.0f, 1.0f, &Settings::haloDark),
    Real("dlss5HaloBright", 0.0f, 1.0f, &Settings::haloBright),
    Choice("dlss5HaloRadius", kMinHaloRadius, kMaxHaloRadius, &Settings::haloRadius),
    Choice("dlss5SupersampleFilter", 0, 1, &Settings::supersample),
    Real("dlss5HdrKnee", kMinKnee, kMaxKnee, &Settings::knee),
    Real("dlss5AdaptiveTarget", kMinAdaptiveTarget, kMaxAdaptiveTarget, &Settings::adaptiveTarget),
    Choice("dlss5BoostTolerance", 0, 2, &Settings::boostTolerance),
    Choice("dlss5DebugView", 0, 3, &Settings::debugView),
    Flag("dlss5CompareSwap", &Settings::compareSwap),
    // autoscale.2: the defaults generation (MigrateDefaults; 3 since alpha.15). Any later value parses and is not
    // migrated.
    Choice("dlss5DefaultsVersion", 0, 65535, &Settings::defaultsVersion),
};

// Before 1.7.0-dlss5.2: "dlss5Intensity" in percent and "dlss5Quality" (1 =
// the 0.85 network extent). Read only when the new key is absent.
inline constexpr const char* kLegacyIntensity = "dlss5Intensity";
inline constexpr const char* kLegacyQuality = "dlss5Quality";

namespace detail
{
inline bool Find(std::string_view content, const char* name, size_t& offset) noexcept
{
    const std::string key = std::string("\"") + name + "\"";
    const size_t at = content.find(key);
    if (at == std::string_view::npos) return false;
    const size_t colon = content.find(':', at + key.size());
    if (colon == std::string_view::npos) return false;
    offset = content.find_first_not_of(" \t\r\n", colon + 1);
    return offset != std::string_view::npos;
}

// The value's end offset, or npos when the value is not of the kind.
inline size_t ParseValue(std::string_view content, size_t offset, Kind kind, float& value) noexcept
{
    if (kind == Kind::eBool)
    {
        if (content.compare(offset, 4, "true") == 0 && ui_status_json::ValueEnd(content, offset + 4))
        { value = 1.0f; return offset + 4; }
        if (content.compare(offset, 5, "false") == 0 && ui_status_json::ValueEnd(content, offset + 5))
        { value = 0.0f; return offset + 5; }
        return std::string_view::npos;
    }
    if (kind == Kind::eUnsigned)
    {
        uint32_t parsed = 0;
        const auto result = std::from_chars(content.data() + offset, content.data() + content.size(), parsed);
        const size_t end = static_cast<size_t>(result.ptr - content.data());
        if (result.ec != std::errc{} || !ui_status_json::ValueEnd(content, end)) return std::string_view::npos;
        value = static_cast<float>(parsed);
        return end;
    }
    float parsed = 0.0f;
    const auto result = std::from_chars(content.data() + offset, content.data() + content.size(), parsed);
    const size_t end = static_cast<size_t>(result.ptr - content.data());
    if (result.ec != std::errc{} || !std::isfinite(parsed) || !ui_status_json::ValueEnd(content, end))
        return std::string_view::npos;
    value = parsed;
    return end;
}

inline std::string Format(const Field& field, const Settings& settings)
{
    if (field.kind == Kind::eBool) return settings.*field.flag ? "true" : "false";
    if (field.kind == Kind::eUnsigned) return std::to_string(settings.*field.integer);
    char text[32]{};
    const auto result = std::to_chars(text, text + sizeof(text) - 1, settings.*field.real);
    return result.ec == std::errc{} ? std::string(text, result.ptr) : std::string("0");
}
}

// Reads every present dlss5* key over `settings`. False when a present key is
// malformed or out of range: the caller keeps its last valid settings. Content
// of an older defaults generation (without dlss5DefaultsVersion: generation 1,
// before autoscale.2) moves to the new defaults where it still held the old
// ones (MigrateDefaults).
inline bool Parse(std::string_view content, Settings& settings) noexcept
{
    Settings parsed = settings;
    size_t marker = 0;
    const bool current = detail::Find(content, "dlss5DefaultsVersion", marker);
    for (const Field& field : kFields)
    {
        size_t offset = 0;
        if (!detail::Find(content, field.key, offset)) continue;
        float value = 0.0f;
        if (detail::ParseValue(content, offset, field.kind, value) == std::string_view::npos
            || value < field.minimum || value > field.maximum) return false;
        if (field.kind == Kind::eBool) parsed.*field.flag = value != 0.0f;
        else if (field.kind == Kind::eUnsigned) parsed.*field.integer = static_cast<uint32_t>(value);
        else parsed.*field.real = value;
    }
    size_t offset = 0;
    float legacy = 0.0f;
    if (!detail::Find(content, "dlss5OverallIntensity", offset) && detail::Find(content, kLegacyIntensity, offset))
    {
        if (detail::ParseValue(content, offset, Kind::eUnsigned, legacy) == std::string_view::npos || legacy > 100.0f)
            return false;
        parsed.intensity = legacy / 100.0f;
    }
    if (!detail::Find(content, "dlss5ResolutionScale", offset) && detail::Find(content, kLegacyQuality, offset))
    {
        if (detail::ParseValue(content, offset, Kind::eUnsigned, legacy) == std::string_view::npos || legacy > 1.0f)
            return false;
        parsed.scale = legacy == 1.0f ? 85.0f : 100.0f;
    }
    const uint32_t from = current ? parsed.defaultsVersion : 1u;
    if (from < kDefaultsVersion) MigrateDefaults(parsed, from);
    settings = Sanitized(parsed);
    return true;
}

// Writes every dlss5* key into a flat JSON object, replacing present values
// and appending absent ones. Legacy keys are removed.
inline bool Write(std::string& content, const Settings& input)
{
    const Settings settings = Sanitized(input);
    for (const char* legacy : {kLegacyIntensity, kLegacyQuality})
    {
        const std::string key = std::string(",\"") + legacy + "\"";
        size_t at = content.find(key);
        if (at == std::string::npos) continue;
        size_t offset = 0;
        float value = 0.0f;
        if (!detail::Find(content, legacy, offset)) return false;
        const size_t end = detail::ParseValue(content, offset, Kind::eUnsigned, value);
        if (end == std::string::npos) return false;
        content.erase(at, end - at);
    }
    for (const Field& field : kFields)
    {
        const std::string value = detail::Format(field, settings);
        size_t offset = 0;
        if (detail::Find(content, field.key, offset))
        {
            float previous = 0.0f;
            // A stored number of another kind (e.g. 1 for a float) is rewritten too.
            size_t end = detail::ParseValue(content, offset, field.kind, previous);
            if (end == std::string::npos) end = detail::ParseValue(content, offset, Kind::eFloat, previous);
            if (end == std::string::npos) return false;
            content.replace(offset, end - offset, value);
        }
        else
        {
            const size_t close = content.find_last_not_of(" \t\r\n");
            if (close == std::string::npos || content[close] != '}') return false;
            content.insert(close, std::string(",\"") + field.key + "\":" + value);
        }
    }
    return true;
}

// The settings as status fields: ",\"dlss5\":true,..." (no braces).
inline std::string StatusFields(const Settings& settings)
{
    std::string fields;
    fields.reserve(768);
    for (const Field& field : kFields)
        fields += std::string(",\"") + field.key + "\":" + detail::Format(field, settings);
    return fields;
}
}
