#pragma once
#include "ampere_gpu.h"
#include "entry_detour.h"
#include "ampere_diagnostics.h"
#include "fg_lifetime_tracker.h"

namespace ampere_backend
{
using LogCallback = ampere_gpu::LogCallback;
enum class FeatureLifetimePhase : uint32_t
{
    eCreated = 1,
    eReleased = 2,
};
struct FeatureLifetimeEvent
{
    FeatureLifetimePhase phase{};
    uintptr_t handle = 0;
    uint64_t lifetime = 0;
    uintptr_t runtime = 0;
    uint64_t runtimeGeneration = 0;
    uintptr_t provider = 0;
    uint64_t providerGeneration = 0;
    uintptr_t wrapper = 0;
    uint64_t wrapperGeneration = 0;
    uint64_t publication = 0;
    uint64_t adapterLuid = 0;
    uint32_t generatedFrameCapacity = 0;
    // Token supplied by the exact pre-Create callback and retained with the
    // successful feature. It is never sampled later during event delivery.
    uint64_t createAttemptToken = 0;
    // GraphicsApi of the NGX entries that created the feature.
    uint32_t api = 0;
};
// The NGX entry family an RTX 30 feature was created through. One process uses
// one family; its runtime and provider Create entries must stay current.
enum class GraphicsApi : uint32_t { eNone = 0, eD3D12 = 1, eVulkan = 2 };
using FeatureLifetimeCallback = void(*)(const FeatureLifetimeEvent&) noexcept;
using PreparationBoundaryResolver = ampere_gpu::PreparationBoundary(*)(HMODULE, uint64_t) noexcept;
void SetPreparationBoundaryResolver(PreparationBoundaryResolver) noexcept;
void SetLogCallback(LogCallback) noexcept;
// Notifications are immutable snapshots queued in native mutation order and
// delivered by the calling threads after gCalls is released. A release can
// therefore be nested inside an outer Streamline free call without reordering
// Created/Released delivery or requiring a permanent dispatch worker.
void SetFeatureLifetimeCallback(FeatureLifetimeCallback) noexcept;
// Valid only for the current thread's in-progress FG Create callback.
void SetCurrentFeatureCreateAttemptToken(uint64_t token) noexcept;
enum class FeatureCreatePendingState : uint8_t { eNone, eCurrent, eReadBusy, eInvalid };
// Read-only and nonblocking. Busy defers control until a later snapshot; it
// never certifies a feature. None still requires all retained-feature checks.
FeatureCreatePendingState FeatureCreatePendingStatus(uint64_t token) noexcept;
bool FeatureCreatePendingCurrent(uint64_t token) noexcept;
// Frame-generation feature lifetime on the NGX runtime D3D12 route, RTX 40 and
// RTX 30 (V1.6.0 live preset switching). A live feature keeps the preset it was
// created with, so a new selection may be latched only while no FG Create is in
// flight and no FG feature lives. The tracker is inexact for the process once a
// successful Create cannot be recorded or a provider FG Create runs outside a
// tracked runtime Create.
fg_lifetime::Snapshot ReadFrameGenerationLiveness() noexcept;
// Nonblocking: no FG Create in flight, no tracked live FG feature, tracker exact.
bool FrameGenerationQuiescent() noexcept;
bool ObserveD3D12Device(void*) noexcept;
// RTX 30 Vulkan hosts: the VkPhysicalDevice selects and verifies the adapter
// (CUDA LUID and compute capability 8.6, as for a D3D12 device). The VkDevice
// the host created on it is the identity NVSDK_NGX_VULKAN_CreateFeature1 must
// carry; a startup observation replaces it, an NGX Init only fills it.
bool ObserveVulkanPhysicalDevice(void*) noexcept;
void ObserveVulkanDevice(void* device, bool startup) noexcept;
// The API whose device confirmed the adapter (eNone before any confirmation).
GraphicsApi DeviceApi() noexcept;
bool PatchProvider(HMODULE, const wchar_t*) noexcept;
bool AdapterVerified() noexcept;
bool Ready() noexcept;
uint32_t FailureCode() noexcept;
ampere_diagnostics::Snapshot Diagnostics() noexcept;
bool EarlyInitObserved() noexcept;
uint64_t PresetQueries() noexcept;
// Current retained provider exposes only the legacy preset value 1. This is
// capability evidence; a real successful Create records the observed value.
bool LegacySinglePreset() noexcept;
uint64_t Evaluations() noexcept;
uint64_t Rejections() noexcept;
uint32_t NativeMaximumGeneratedFrames() noexcept;
uint32_t NativeMaximumReadResult() noexcept;
uint32_t StartupMaximumGeneratedFrames() noexcept;
uint32_t PresentationBuffers() noexcept;
uint32_t CertifiedMaximumGeneratedFrames() noexcept;
void ObserveActiveWrapper(HMODULE, uint64_t generation) noexcept;
int32_t LastCreateCount() noexcept;
uint64_t CreatedFeatures() noexcept;
uint64_t SubmittedBatches() noexcept;
uint64_t FailedBatches() noexcept;
uint64_t RecoveredBatches() noexcept;
uint64_t OutputReuseLatches() noexcept;

// All module work occurs on loader return or ordinary calls, never from the
// loader notification callback. Startup is serialized with feature creation.
void BeginStartup() noexcept;
void ObserveEarlyInit() noexcept;
void BeginInit() noexcept;
void EndInit() noexcept;
bool BeginNativePreparation() noexcept;
void EndNativePreparation() noexcept;
bool BeforeFirstFeatureCreate() noexcept;
void EndStartup(bool accepted) noexcept;
void ObserveModule(HMODULE, const wchar_t*, uint64_t generation,
    bool wrapper, bool provider, bool runtime) noexcept;
bool InstallRoute(HMODULE, const wchar_t*, uint64_t generation, bool runtime,
    entry_detour::ForwardPreCall beforeCreate,
    entry_detour::ForwardPreCall beforeEvaluate) noexcept;

// One patcher-owned NGX Vulkan Create/Create1/Evaluate forwarding entry. For a
// runtime entry the rejected target becomes this route's RTX 30 thunk: the
// patcher's gate returns false on RTX 30 to divert there, and the thunk rejects
// every call (FeatureNotSupported, as `rejected`) when RTX 30 is not selected.
// A provider entry keeps `rejected`; its RTX 30 half is VulkanProviderGate.
bool InstallVulkanEntry(HMODULE owner, uint64_t generation, bool runtime, entry_detour::Kind kind,
    void* target, entry_detour::ForwardPreCall preCall, entry_detour::ForwardGate gate, void* rejected,
    const entry_detour::InstallOptions& options, entry_detour::Handle* handle) noexcept;
// RTX 30 systems only (ampere_provider_arch::Armed): NVSDK_NGX_VULKAN_ReleaseFeature
// on the provider and runtime, and the runtime's Vulkan requirements and
// capability-parameter thunks. Returns whether every present export is covered.
bool InstallVulkanRoute(HMODULE, const wchar_t*, uint64_t generation, bool runtime) noexcept;
// The provider half of an RTX 30 Vulkan Create/Create1/Evaluate, called by the
// patcher's gate only while RTX 30 is selected; the relay then runs the
// patcher's own pre-call. Admits only a call nested in a tracked runtime call.
bool WINAPI VulkanProviderGate(void*, uintptr_t, const void*, void*, uintptr_t, uintptr_t,
    entry_detour::Handle, const void* caller) noexcept;
// The provider's SM86 program is prepared and its Vulkan entries are current.
bool VulkanProviderPrepared(HMODULE) noexcept;
}
