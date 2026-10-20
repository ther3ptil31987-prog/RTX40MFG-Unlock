#pragma once

#include <Windows.h>

#include <cstddef>
#include <cstdint>
#include <cstring>

// The RTX 30 Vulkan route's view of a DLSS-G output. DLSSG.OutputInterpolated
// is an NVSDK_NGX_Resource_VK* on Vulkan; only the prefix read here is
// represented, so the core gains no Vulkan SDK header (the layout is checked
// against nvsdk_ngx_defs_vk.h by tests/ampere_runtime/vulkan_route_tests.cpp).
namespace ampere_vulkan_output
{
// NVSDK_NGX_ImageViewInfo_VK (the resource union's image-view member), then
// NVSDK_NGX_Resource_VK::Type. NVSDK_NGX_RESOURCE_VK_TYPE_VK_IMAGEVIEW is 0.
struct ImageView
{
    uint64_t imageView = 0;
    uint64_t image = 0;
    uint32_t aspectMask = 0, baseMipLevel = 0, levelCount = 0, baseArrayLayer = 0, layerCount = 0;
    uint32_t format = 0;
    uint32_t width = 0, height = 0;
    uint32_t type = 0;
};
static_assert(offsetof(ImageView, image) == 8);
static_assert(offsetof(ImageView, aspectMask) == 16);
static_assert(offsetof(ImageView, format) == 36);
static_assert(offsetof(ImageView, width) == 40);
static_assert(offsetof(ImageView, height) == 44);
static_assert(offsetof(ImageView, type) == 48);
constexpr uint32_t kImageViewType = 0;
constexpr uint32_t kRemaining = UINT32_MAX; // VK_REMAINING_MIP_LEVELS / _ARRAY_LAYERS

// The output identity (VkImage) and the fixed properties a feature keeps.
struct Output
{
    uint64_t image = 0;
    uint32_t format = 0, width = 0, height = 0;
};

// A single-level, single-layer image view with an image, format and size.
// The view descriptor may be a per-call copy; the VkImage is the identity.
inline bool Valid(const ImageView& view) noexcept
{
    return view.type == kImageViewType && view.image && view.imageView && view.format
        && view.width && view.height && !view.baseMipLevel && !view.baseArrayLayer
        && (view.levelCount == 1 || view.levelCount == kRemaining)
        && (view.layerCount == 1 || view.layerCount == kRemaining);
}

inline bool Read(const void* resource, Output& output) noexcept
{
    output = {};
    if (!resource) return false;
    ImageView view{};
    __try { std::memcpy(&view, resource, sizeof(view)); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    if (!Valid(view)) return false;
    output = {view.image, view.format, view.width, view.height};
    return true;
}
}
