// RTX 30 Vulkan route: the NGX Vulkan output view, the GPU selection policy for
// Vulkan Create/Evaluate, and the VkPhysicalDevice LUID query. With MFG_TEST_NGX_VK
// the prefixes are checked against nvsdk_ngx_defs_vk.h and vulkan_core.h.
// Argument "gpu": also query every Vulkan physical device on this machine and
// match its LUID to a DXGI adapter (exit 2 when no Vulkan device is present).
#include "ampere_vulkan_output.h"
#include "gpu_selection_policy.h"
#include "vulkan_physical_device.h"
#if MFG_TEST_NGX_VK
#include <vulkan/vulkan.h>
#include <nvsdk_ngx_defs_vk.h>
#endif
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace
{
unsigned checks = 0, failures = 0;
void Check(bool ok, const char* text)
{
    ++checks;
    failures += !ok;
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", text);
}

#if MFG_TEST_NGX_VK
using ampere_vulkan_output::ImageView;
static_assert(offsetof(NVSDK_NGX_Resource_VK, Resource) == 0);
static_assert(offsetof(NVSDK_NGX_ImageViewInfo_VK, ImageView) == offsetof(ImageView, imageView));
static_assert(offsetof(NVSDK_NGX_ImageViewInfo_VK, Image) == offsetof(ImageView, image));
static_assert(offsetof(NVSDK_NGX_ImageViewInfo_VK, SubresourceRange) == offsetof(ImageView, aspectMask));
static_assert(offsetof(VkImageSubresourceRange, baseMipLevel) == offsetof(ImageView, baseMipLevel) - offsetof(ImageView, aspectMask));
static_assert(offsetof(VkImageSubresourceRange, levelCount) == offsetof(ImageView, levelCount) - offsetof(ImageView, aspectMask));
static_assert(offsetof(VkImageSubresourceRange, baseArrayLayer) == offsetof(ImageView, baseArrayLayer) - offsetof(ImageView, aspectMask));
static_assert(offsetof(VkImageSubresourceRange, layerCount) == offsetof(ImageView, layerCount) - offsetof(ImageView, aspectMask));
static_assert(offsetof(NVSDK_NGX_ImageViewInfo_VK, Format) == offsetof(ImageView, format));
static_assert(offsetof(NVSDK_NGX_ImageViewInfo_VK, Width) == offsetof(ImageView, width));
static_assert(offsetof(NVSDK_NGX_ImageViewInfo_VK, Height) == offsetof(ImageView, height));
static_assert(offsetof(NVSDK_NGX_Resource_VK, Type) == offsetof(ImageView, type));
static_assert(sizeof(NVSDK_NGX_Resource_VK) == sizeof(ImageView));
static_assert(NVSDK_NGX_RESOURCE_VK_TYPE_VK_IMAGEVIEW == ampere_vulkan_output::kImageViewType);
static_assert(VK_REMAINING_MIP_LEVELS == ampere_vulkan_output::kRemaining);
static_assert(VK_REMAINING_ARRAY_LAYERS == ampere_vulkan_output::kRemaining);
static_assert(offsetof(VkPhysicalDeviceIDProperties, pNext) == offsetof(vulkan_physical_device::IdProperties, pNext));
static_assert(offsetof(VkPhysicalDeviceIDProperties, deviceLUID) == offsetof(vulkan_physical_device::IdProperties, deviceLuid));
static_assert(offsetof(VkPhysicalDeviceIDProperties, deviceNodeMask) == offsetof(vulkan_physical_device::IdProperties, deviceNodeMask));
static_assert(offsetof(VkPhysicalDeviceIDProperties, deviceLUIDValid) == offsetof(vulkan_physical_device::IdProperties, deviceLuidValid));
static_assert(sizeof(VkPhysicalDeviceIDProperties) == sizeof(vulkan_physical_device::IdProperties));
static_assert(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES == 1000071004);
static_assert(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2 == 1000059001);
static_assert(offsetof(VkPhysicalDeviceProperties2, properties) == offsetof(vulkan_physical_device::Properties2, properties));
static_assert(sizeof(VkPhysicalDeviceProperties) <= sizeof(vulkan_physical_device::Properties2::properties));
#endif

ampere_vulkan_output::ImageView GoodView()
{
    ampere_vulkan_output::ImageView view{};
    view.imageView = 0x1000;
    view.image = 0x2000;
    view.aspectMask = 1;
    view.levelCount = 1;
    view.layerCount = 1;
    view.format = 44; // VK_FORMAT_B8G8R8A8_UNORM
    view.width = 3840;
    view.height = 2160;
    return view;
}

void OutputTests()
{
    using namespace ampere_vulkan_output;
    // A padded copy as large as NVSDK_NGX_Resource_VK, ReadWrite set.
    struct Resource { ImageView view; } resource{GoodView()};
    reinterpret_cast<uint8_t*>(&resource.view.type)[4] = 1;
    Output output{};
    Check(Read(&resource, output) && output.image == 0x2000 && output.format == 44
        && output.width == 3840 && output.height == 2160,
        "image view output yields its VkImage identity, format and size");
    auto variant = GoodView();
    variant.levelCount = kRemaining;
    variant.layerCount = kRemaining;
    Check(Valid(variant), "VK_REMAINING mip levels and array layers name the single base level/layer");
    const auto refused = [](auto change, const char* text) {
        auto view = GoodView();
        change(view);
        Output out{};
        Check(!Valid(view) && !Read(&view, out) && !out.image, text);
    };
    refused([](ImageView& v) { v.type = 1; }, "a buffer resource is never an FG output");
    refused([](ImageView& v) { v.image = 0; }, "a view without its VkImage has no identity");
    refused([](ImageView& v) { v.imageView = 0; }, "a missing VkImageView is refused");
    refused([](ImageView& v) { v.format = 0; }, "VK_FORMAT_UNDEFINED is refused");
    refused([](ImageView& v) { v.width = 0; }, "a zero width is refused");
    refused([](ImageView& v) { v.height = 0; }, "a zero height is refused");
    refused([](ImageView& v) { v.baseMipLevel = 1; }, "a view of a lower mip level is refused");
    refused([](ImageView& v) { v.levelCount = 2; }, "a view of several mip levels is refused");
    refused([](ImageView& v) { v.baseArrayLayer = 1; }, "a view of a later array layer is refused");
    refused([](ImageView& v) { v.layerCount = 6; }, "a layered view is refused");
    Output out{};
    Check(!Read(nullptr, out), "a missing output pointer is refused");
    void* reserved = VirtualAlloc(nullptr, 4096, MEM_RESERVE, PAGE_NOACCESS);
    Check(reserved && !Read(reserved, out) && !out.image, "an unreadable descriptor is refused without a fault");
    if (reserved) VirtualFree(reserved, 0, MEM_RELEASE);
}

void PolicyTests()
{
    using gpu_dispatch::Family;
    using gpu_dispatch::AllowVulkanCreate;
    using gpu_dispatch::AllowVulkanEvaluate;
    Check(AllowVulkanCreate(Family::eAmpere, true, true), "verified RTX 30 Vulkan FG Create reaches the Ampere route");
    Check(!AllowVulkanCreate(Family::eAmpere, true, false), "unverified RTX 30 Vulkan FG Create is refused");
    Check(AllowVulkanCreate(Family::eAda, true, true) && !AllowVulkanCreate(Family::eAda, true, false),
        "RTX 40 Vulkan FG Create rules are unchanged");
    Check(AllowVulkanCreate(Family::eUnknown, true, false), "an unselected GPU still forwards FG Create natively");
    Check(!AllowVulkanCreate(Family::eConflict, true, true) && AllowVulkanCreate(Family::eConflict, false, false),
        "a selection conflict refuses FG Create only");
    Check(AllowVulkanEvaluate(Family::eAmpere, true, true) && !AllowVulkanEvaluate(Family::eAmpere, true, false),
        "RTX 30 provider Evaluate needs the verified Vulkan adapter");
    Check(AllowVulkanEvaluate(Family::eAmpere, false, false), "shared-runtime SR/RR Evaluate is not blocked here");
    Check(!AllowVulkanEvaluate(Family::eConflict, false, true), "a selection conflict refuses every Evaluate");
}

// Every Vulkan physical device's LUID matches a DXGI adapter of the same LUID.
int GpuTests()
{
    HMODULE loader = LoadLibraryExW(L"vulkan-1.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!loader) { std::printf("SETUP no system Vulkan loader\n"); return 2; }
    using GetInstanceProcAddr = void* (WINAPI*)(void*, const char*);
    auto* gipa = reinterpret_cast<GetInstanceProcAddr>(GetProcAddress(loader, "vkGetInstanceProcAddr"));
    struct ApplicationInfo { uint32_t sType = 0; const void* next = nullptr; const char* name = "MFGVulkanRouteTests";
        uint32_t version = 1; const char* engine = nullptr; uint32_t engineVersion = 0; uint32_t api = (1u << 22) | (1u << 12); };
    struct InstanceInfo { uint32_t sType = 1; const void* next = nullptr; uint32_t flags = 0;
        const ApplicationInfo* application = nullptr; uint32_t layers = 0; const char* const* layerNames = nullptr;
        uint32_t extensions = 0; const char* const* extensionNames = nullptr; };
    using CreateInstance = int32_t (WINAPI*)(const InstanceInfo*, const void*, void**);
    using DestroyInstance = void (WINAPI*)(void*, const void*);
    using EnumeratePhysicalDevices = int32_t (WINAPI*)(void*, uint32_t*, void**);
    auto* create = gipa ? reinterpret_cast<CreateInstance>(gipa(nullptr, "vkCreateInstance")) : nullptr;
    ApplicationInfo application{};
    InstanceInfo info{};
    info.application = &application;
    void* instance = nullptr;
    if (!create || create(&info, nullptr, &instance) != 0 || !instance)
    { std::printf("SETUP no Vulkan 1.1 instance\n"); FreeLibrary(loader); return 2; }
    auto* destroy = reinterpret_cast<DestroyInstance>(gipa(instance, "vkDestroyInstance"));
    auto* enumerate = reinterpret_cast<EnumeratePhysicalDevices>(gipa(instance, "vkEnumeratePhysicalDevices"));
    uint32_t count = 0;
    std::vector<void*> devices;
    if (enumerate && enumerate(instance, &count, nullptr) == 0 && count)
    {
        devices.resize(count);
        if (enumerate(instance, &count, devices.data()) < 0) devices.clear();
        devices.resize(count);
    }
    Microsoft::WRL::ComPtr<IDXGIFactory4> factory;
    const bool dxgi = SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)));
    unsigned matched = 0, nvidia = 0;
    for (void* device : devices)
    {
        LUID luid{};
        const bool queried = vulkan_physical_device::QueryLuid(device, luid);
        Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
        DXGI_ADAPTER_DESC1 description{};
        const bool found = queried && dxgi && SUCCEEDED(factory->EnumAdapterByLuid(luid, IID_PPV_ARGS(&adapter)))
            && SUCCEEDED(adapter->GetDesc1(&description));
        std::printf("INFO physical device %p luid=%08lX:%08lX dxgi=%d vendor=%04X %ls\n", device,
            static_cast<unsigned long>(luid.HighPart), luid.LowPart, found ? 1 : 0, description.VendorId,
            found ? description.Description : L"");
        matched += found;
        nvidia += found && description.VendorId == 0x10de;
    }
    Check(!devices.empty() && matched == devices.size(),
        "every Vulkan physical device reports the LUID of a DXGI adapter");
    Check(nvidia > 0, "the NVIDIA adapter is identified through its VkPhysicalDevice LUID");
    LUID none{};
    Check(!vulkan_physical_device::QueryLuid(nullptr, none), "a missing physical device reports no LUID");
    if (destroy) destroy(instance, nullptr);
    FreeLibrary(loader);
    return 0;
}
}

int wmain(int argc, wchar_t** argv)
{
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    OutputTests();
    PolicyTests();
    if (argc > 1 && std::wstring(argv[1]) == L"gpu")
    {
        const int setup = GpuTests();
        if (setup == 2) return 2;
    }
    std::printf("%u checks, %u failures\n", checks, failures);
    return failures ? 1 : 0;
}
