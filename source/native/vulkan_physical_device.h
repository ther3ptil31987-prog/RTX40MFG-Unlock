#pragma once

#include <Windows.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

// Adapter identity of a VkPhysicalDevice for the RTX 30 backend and the Ada
// midpoint fix. The system Vulkan loader is resolved dynamically, so the
// universal core gains no Vulkan SDK header or loader import. Only the prefix of
// the two structures consumed by vkGetPhysicalDeviceProperties2 is represented;
// its fixed ABI offsets are checked below. The opaque properties storage exceeds
// VkPhysicalDeviceProperties.
namespace vulkan_physical_device
{
struct IdProperties
{
    uint32_t sType = 1000071004u; // VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES
    uint32_t padding = 0;
    void* pNext = nullptr;
    std::array<uint8_t, 16> deviceUuid{};
    std::array<uint8_t, 16> driverUuid{};
    std::array<uint8_t, 8> deviceLuid{};
    uint32_t deviceNodeMask = 0;
    uint32_t deviceLuidValid = 0;
};

struct Properties2
{
    uint32_t sType = 1000059001u; // VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2
    uint32_t padding = 0;
    void* pNext = nullptr;
    alignas(8) std::array<uint8_t, 1024> properties{};
};

static_assert(offsetof(IdProperties, pNext) == 8);
static_assert(offsetof(IdProperties, deviceLuid) == 48);
static_assert(offsetof(IdProperties, deviceLuidValid) == 60);
static_assert(offsetof(Properties2, pNext) == 8);
static_assert(offsetof(Properties2, properties) == 16);

// False when the loader, the query or the LUID-valid flag is unavailable; the
// LUID may still be zero. Never call under the loader lock: it loads the system
// vulkan-1.dll.
inline bool QueryReportedLuid(void* physicalDevice, LUID& luid) noexcept
{
    luid = {};
    if (!physicalDevice) return false;
    using GetProperties2 = void (WINAPI*)(void*, Properties2*);
    HMODULE vulkan = LoadLibraryExW(L"vulkan-1.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!vulkan) return false;
    auto* getProperties = reinterpret_cast<GetProperties2>(GetProcAddress(vulkan, "vkGetPhysicalDeviceProperties2"));
    if (!getProperties)
        getProperties = reinterpret_cast<GetProperties2>(GetProcAddress(vulkan, "vkGetPhysicalDeviceProperties2KHR"));
    IdProperties identity{};
    Properties2 properties{};
    properties.pNext = &identity;
    bool queried = false;
    if (getProperties)
    {
        __try
        {
            getProperties(physicalDevice, &properties);
            queried = identity.deviceLuidValid != 0;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { queried = false; }
    }
    FreeLibrary(vulkan);
    if (!queried) return false;
    std::memcpy(&luid, identity.deviceLuid.data(), sizeof(luid));
    return true;
}

// As QueryReportedLuid, and false for a zero LUID.
inline bool QueryLuid(void* physicalDevice, LUID& luid) noexcept
{
    return QueryReportedLuid(physicalDevice, luid) && (luid.LowPart != 0 || luid.HighPart != 0);
}
}
