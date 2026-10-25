#pragma once
// xvk: the minimal D3D12-on-Vulkan layer that runs Intel's unmodified XeSS-FG/XeLL CPU code on a game's own
// Vulkan device (XeMFG on Vulkan, V1.6.2 vkxefg.1; research board #515-#519). It implements only the D3D12/DXGI
// subset XeFG uses and fails closed on anything else. Compute programs come from embedded SPIR-V, translated
// from the exact DXIL and matched by the DXIL's SHA-256 (xess/vulkan/generate_xvk_programs.py).
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#ifndef VK_NO_PROTOTYPES
#define VK_NO_PROTOTYPES
#endif
#ifndef VK_USE_PLATFORM_WIN32_KHR
#define VK_USE_PLATFORM_WIN32_KHR
#endif
#include <vulkan/vulkan.h>
#include <cstdint>

namespace xvk
{
struct QueueId
{
    uint32_t family;
    uint32_t index;
};

struct DeviceDesc
{
    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;                   // created with the extensions and features from Requirements()
    PFN_vkGetInstanceProcAddr getInstanceProcAddr = nullptr;   // the loader's own entry, never an interposed one
    uint32_t queueCount = 0;                            // queues only the layer submits to, all in one family;
    const QueueId* queues = nullptr;                    // the last one presents, the others back D3D12 queues
    HMODULE programModule = nullptr;                    // module holding the embedded program set
    bool calibratedTimestamps = false;                  // VK_KHR/EXT_calibrated_timestamps enabled on the device
    bool presentIds = false;                            // vkreflex.1: VK_KHR_present_id with its presentId feature on
    // Optional: device functions to use instead of vkGetDeviceProcAddr's (a host whose swap chain and present
    // entries are hooked passes its originals, so the layer's own calls never re-enter its hooks). Null = loader's.
    PFN_vkVoidFunction (VKAPI_PTR* deviceProcOverride)(void* user, const char* name) = nullptr;
    void* deviceProcOverrideUser = nullptr;
};

// What the device needs. The SPIR-V is version 1.6, so the device must run at Vulkan 1.3 or later.
constexpr uint32_t kRequiredApiVersion = VK_API_VERSION_1_3;
// Device extensions beyond VK_KHR_swapchain (null-terminated).
const char* const* RequiredDeviceExtensions() noexcept;
// Every feature the layer needs, as Vulkan 1.2/1.3 feature bits.
struct RequiredFeatures
{
    bool timelineSemaphore = true;
    bool synchronization2 = true;
    bool maintenance4 = true;
    bool shaderIntegerDotProduct = true;
};

HRESULT CreateDevice(const DeviceDesc& desc, REFIID riid, void** device) noexcept;
HRESULT CreateFactory(ID3D12Device* device, REFIID riid, void** factory) noexcept;
// The game's surface for a window: swap chains the layer creates on that window use it and never destroy it.
// vkswapext.1: `exclusive` is the game's VK_EXT_full_screen_exclusive mode for it (DEFAULT: none asked for); the
// layer's swap chains on the surface are created with it. mailbox.1: `presentMode` is the mode the game asked for; a
// game on MAILBOX keeps it for XeFG's tearing presents (otherwise IMMEDIATE, as before).
void RegisterSurface(ID3D12Device* device, HWND window, VkSurfaceKHR surface,
                     VkFullScreenExclusiveEXT exclusive = VK_FULL_SCREEN_EXCLUSIVE_DEFAULT_EXT,
                     VkPresentModeKHR presentMode = VK_PRESENT_MODE_IMMEDIATE_KHR) noexcept;
void ForgetSurface(HWND window) noexcept;
// A layer swap chain on the registered surface could not create its VkSwapchainKHR (its frames are dropped).
bool SurfaceBroken(HWND window) noexcept;
// Successful vkQueuePresentKHR calls by every layer swap chain.
uint64_t PresentCount() noexcept;
// Programs the embedded set holds, and how many creations failed closed for a missing program.
size_t EmbeddedProgramCount(HMODULE module) noexcept;
uint64_t MissingProgramCount() noexcept;

// vkreflex.1: NVIDIA Reflex (VK_NV_low_latency2) on the layer's swap chains and AMD Anti-Lag 2 (VK_AMD_anti_lag) on its
// device, for the host's low-latency output. While Reflex is on, a layer swap chain created on a device with the
// extension's functions gets low latency mode; the sleep mode goes to every such chain, now and when one is created,
// and its presents carry out-of-band present markers (the real frame also its present id) for the batch's frame.
struct ReflexMode
{
    bool on = false;
    bool lowLatency = false;
    bool boost = false;
    uint32_t minimumIntervalUs = 0;   // the FPS limit, as the base frame interval
    bool operator==(const ReflexMode&) const = default;
};
void SetReflexMode(ID3D12Device* device, const ReflexMode& mode) noexcept;
// Reflex's sleep for the next frame on the window's low-latency swap chain, waited for at most `timeoutNs`; false when
// the window has none (the caller sleeps another way).
bool ReflexSleep(HWND window, uint64_t timeoutNs) noexcept;
void ReflexMarker(HWND window, uint64_t frame, VkLatencyMarkerNV marker) noexcept;
// The host's frame id of XeFG's first frame on the window's next layer swap chain: XeFG numbers its frames from 0 per
// context (the low 32 bits of its present's frame), the host's ids run on across the game's swap chains.
void SetFrameBase(HWND window, uint64_t base) noexcept;
// Anti-Lag 2's input or present stage for `frame` (maxFps 0: no limit); false without the extension's function.
bool AntiLagUpdate(ID3D12Device* device, bool on, uint32_t maxFps, VkAntiLagStageAMD stage, uint64_t frame) noexcept;
}
