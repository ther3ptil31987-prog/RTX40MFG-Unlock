#pragma once
#include <Windows.h>
#include <cstdint>
#include <string>

// Display policy only. This code never calls a provider. beta.8: XeSS MFG runs under
// OptiScaler only and OptiScaler's menu owns its controls, so the menu shows status here.
namespace xess_ui
{
struct State
{
    BOOL available = FALSE, hooksActive = FALSE, optiScaler = FALSE;
    BOOL initialized = FALSE, enabled = FALSE;
    uint32_t applied = 0, capacity = 0, framesPresented = 0, adapterVendor = 0, gpuVendorId = 0;
    uint32_t modelActive = 0; // 0 none yet, 1 our port of Intel's MFG model, 2 XeFG's 2x model
    std::string runtimeFailure, status, adapter;
    std::string activeFrameGeneration = "none";
};

// Use the frontend's existing scalar readers. Absent fields stay false or zero,
// so an older native snapshot cannot imply an applied count.
template<class BoolReader, class IntegerReader, class StringReader>
State Read(const std::string& text, BoolReader boolean, IntegerReader integer, StringReader string)
{
    State next;
    integer(text, "gpuVendorId", next.gpuVendorId);
    string(text, "activeFrameGeneration", next.activeFrameGeneration);
    boolean(text, "optiScalerDetected", next.optiScaler);
    if (!boolean(text, "xessAvailable", next.available) || !next.available) return next;
    boolean(text, "xessHooksActive", next.hooksActive);
    boolean(text, "xessInitialized", next.initialized);
    boolean(text, "xessEnabled", next.enabled);
    integer(text, "xessApplied", next.applied);
    integer(text, "xessCapacity", next.capacity);
    integer(text, "xessFramesPresented", next.framesPresented);
    integer(text, "xessAdapterVendor", next.adapterVendor);
    integer(text, "xessModelActive", next.modelActive);
    string(text, "xessRuntimeFailure", next.runtimeFailure);
    string(text, "xessStatus", next.status);
    string(text, "xessAdapter", next.adapter);
    return next;
}

inline uint32_t Vendor(const State& state) noexcept
{
    return state.gpuVendorId ? state.gpuVendorId : state.adapterVendor;
}
// Which settings the menu shows: the frame generation selected in the game.
// XeSS frame generation on shows XeSS and DLSS frame generation on shows DLSS,
// on any GPU. With frame generation off the menu keeps the last one the game
// turned on this session (lastActive). Before the game has turned either on,
// XeSS shows on non-NVIDIA GPUs, in games offering only XeSS, and on NVIDIA
// GPUs the DLSS backend checked and found without DLSS MFG (gpuFamily 1 Ada,
// 2 Ampere experimental); DLSS shows otherwise.
enum class Section { Dlss, Xess };
constexpr uint32_t kNvidiaVendor = 0x10de;
inline bool DlssMfgFamily(uint32_t gpuFamily) noexcept { return gpuFamily == 1 || gpuFamily == 2; }
inline Section SettingsSection(const State& state, const std::string& lastActive, bool dlssOffered,
    uint32_t gpuFamily) noexcept
{
    const std::string& selected = state.activeFrameGeneration != "none" ? state.activeFrameGeneration : lastActive;
    if (selected == "xess") return Section::Xess;
    if (selected == "dlss") return Section::Dlss;
    const uint32_t vendor = Vendor(state);
    if (vendor && vendor != kNvidiaVendor) return Section::Xess;
    if (!state.available || !state.hooksActive) return Section::Dlss;
    if (!dlssOffered) return Section::Xess;
    // gpuVendorId is published only after the DLSS backend's device check, so
    // an unchecked GPU keeps the DLSS settings it may still need before FG is on.
    return state.gpuVendorId == kNvidiaVendor && !DlssMfgFamily(gpuFamily) ? Section::Xess : Section::Dlss;
}
inline bool AppliedKnown(const State& state) noexcept
{
    // An accepted count needs a matching allocation.
    return state.initialized && state.applied >= 2 && state.applied <= 6 && state.capacity >= state.applied - 1;
}

enum class VsyncReadback { Pending, Overridden, AlreadyOff };
constexpr VsyncReadback VsyncResult(bool available, bool observed, bool overridden,
    uint32_t originalInterval, uint32_t submittedInterval) noexcept
{
    if (!available || !observed || submittedInterval != 0) return VsyncReadback::Pending;
    if (overridden && originalInterval != 0) return VsyncReadback::Overridden;
    if (!overridden && originalInterval == 0) return VsyncReadback::AlreadyOff;
    return VsyncReadback::Pending;
}
}
