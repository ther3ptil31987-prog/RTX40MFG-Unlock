#pragma once

#include "dlss5_policy.h"

#include <Windows.h>
#include <d3d12.h>
#include <nvsdk_ngx.h>

// Writes the DLSSNR Create and Evaluate keys (dlss5_policy.h) on a parameter
// object of ours, with the value types the provider reads. Shared by the
// runtime module and the provider harness.
namespace dlss5
{
namespace detail
{
inline void SetRect(NVSDK_NGX_Parameter* parameters, const keys::ResourceKeys& names, uintptr_t resource,
    const Rect& rect) noexcept
{
    // The provider reads resources with Get(void**) and subrects with Get(int*).
    parameters->Set(names.resource, reinterpret_cast<void*>(resource));
    parameters->Set(names.rect[0], static_cast<int>(rect.x));
    parameters->Set(names.rect[1], static_cast<int>(rect.y));
    parameters->Set(names.rect[2], static_cast<int>(rect.width));
    parameters->Set(names.rect[3], static_cast<int>(rect.height));
}
}

// The provider's image controls. They are written with every Create and
// Evaluate, so a change applies to the next evaluation without a new feature.
// `settings` are the pass's own (PassSettings).
inline void WriteControls(NVSDK_NGX_Parameter* parameters, const Settings& settings) noexcept
{
    parameters->Set(keys::kEnabled, 1);
    parameters->Set(keys::kIntensity, settings.intensity);
    parameters->Set(keys::kStyle, static_cast<int>(settings.model));
    parameters->Set(keys::kLocalStructure, settings.structure);
    parameters->Set(keys::kLocalTone, settings.tone);
    parameters->Set(keys::kSkinStructure, settings.character);
    parameters->Set(keys::kAutoMask, settings.characterMask ? 1 : 0);
    parameters->Set(keys::kUiCorrection, UiCorrectionValue(settings, static_cast<Stage>(settings.stage)));
}

// A fresh Create for the processing extent (Resolution Scale applied by the
// caller): network at that extent (MaxQuality, ratio 1), the preset hint (int)
// and node masks (unsigned).
inline bool WriteCreate(NVSDK_NGX_Parameter* parameters, uint32_t width, uint32_t height,
    const Settings& settings) noexcept
{
    if (!parameters || !width || !height) return false;
    __try
    {
        parameters->Set(keys::kWidth, width);
        parameters->Set(keys::kHeight, height);
        parameters->Set(keys::kPerfQuality, static_cast<unsigned int>(kQualityFull));
        parameters->Set(keys::kPreset, static_cast<int>(settings.preset));
        parameters->Set("Width", width);
        parameters->Set("Height", height);
        parameters->Set("OutWidth", width);
        parameters->Set("OutHeight", height);
        parameters->Set(keys::kScalingRatio, 1.0f);
        WriteControls(parameters, settings);
        parameters->Set(keys::kCreationNodeMask, 1u);
        parameters->Set(keys::kVisibilityNodeMask, 1u);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// Before Create of network pass `pass`: that pass's controls (its Model and
// strengths when later passes have their own).
inline bool WritePassControls(NVSDK_NGX_Parameter* parameters, const Settings& settings, uint32_t pass) noexcept
{
    if (!parameters) return false;
    __try
    {
        WriteControls(parameters, PassSettings(settings, pass));
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// `settings` are the pass's own (PassSettings).
inline bool WriteEvaluate(NVSDK_NGX_Parameter* parameters, const Frame& frame, const Settings& settings) noexcept
{
    if (!parameters) return false;
    __try
    {
        detail::SetRect(parameters, keys::kColorKeys, frame.color, frame.colorRect);
        detail::SetRect(parameters, keys::kOutputKeys, frame.output, frame.outputRect);
        detail::SetRect(parameters, keys::kMotionKeys, frame.motion, frame.motionRect);
        detail::SetRect(parameters, keys::kDepthKeys, frame.depth, frame.depthRect);
        parameters->Set(keys::kMotionScaleX, frame.motionScaleX);
        parameters->Set(keys::kMotionScaleY, frame.motionScaleY);
        parameters->Set(keys::kDepthInverted, static_cast<int>(frame.depthInverted));
        parameters->Set(keys::kReset, static_cast<int>(frame.reset));
        WriteControls(parameters, settings);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// NGX capability keys for DLSSNR (-1 = absent).
struct Capability
{
    int available = -1;
    int needsUpdatedDriver = -1;
    int minDriverMajor = -1;
    int minDriverMinor = -1;
    int featureInitResult = -1;
};

inline Capability ReadCapability(const NVSDK_NGX_Parameter* parameters) noexcept
{
    Capability capability{};
    if (!parameters) return capability;
    const auto read = [&](const char* name, int& out) noexcept
    {
        int value = 0;
        unsigned int unsignedValue = 0;
        if (parameters->Get(name, &value) == NVSDK_NGX_Result_Success) out = value;
        else if (parameters->Get(name, &unsignedValue) == NVSDK_NGX_Result_Success) out = static_cast<int>(unsignedValue);
    };
    __try
    {
        read(keys::kAvailable, capability.available);
        read(keys::kNeedsUpdatedDriver, capability.needsUpdatedDriver);
        read(keys::kMinDriverMajor, capability.minDriverMajor);
        read(keys::kMinDriverMinor, capability.minDriverMinor);
        read(keys::kFeatureInitResult, capability.featureInitResult);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
    return capability;
}
}
