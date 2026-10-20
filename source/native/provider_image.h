#pragma once
// Stateless reads of a mapped provider image, shared by the Ada midpoint fix and the RTX 30 backend,
// and of a MinHook route (the overlay and GPU name detours).
// SafeCopy/SafeRead tolerate unreadable addresses (SEH); the others assume validated bounds.
#include <Windows.h>
#include <cstdint>
#include <cstring>

namespace provider_image
{
constexpr bool SameName(const char* a, const char* b) noexcept
{
    if (!a || !b) return false;
    for (; *a || *b; ++a, ++b)
        if (*a != *b) return false;
    return true;
}

inline bool SafeCopy(void* destination, const void* source, size_t bytes) noexcept
{
    if (!destination || !source || bytes == 0)
        return false;
    __try
    {
        std::memcpy(destination, source, bytes);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

template <typename T>
inline bool SafeRead(uintptr_t address, T& value) noexcept
{
    return SafeCopy(&value, reinterpret_cast<const void*>(address),
        sizeof(value));
}

inline uint16_t ReadU16(const uint8_t* bytes) noexcept
{
    uint16_t value = 0;
    std::memcpy(&value, bytes, sizeof(value));
    return value;
}

inline uint32_t ReadU32(const uint8_t* bytes) noexcept
{
    uint32_t value = 0;
    std::memcpy(&value, bytes, sizeof(value));
    return value;
}

inline uint64_t ReadU64(const uint8_t* bytes) noexcept
{
    uint64_t value = 0;
    std::memcpy(&value, bytes, sizeof(value));
    return value;
}

inline bool ImageSize(HMODULE module, uint32_t& imageSize) noexcept
{
    imageSize = 0;
    const uintptr_t base = reinterpret_cast<uintptr_t>(module);
    IMAGE_DOS_HEADER dos{};
    if (!base || !SafeRead(base, dos) || dos.e_magic != IMAGE_DOS_SIGNATURE
        || dos.e_lfanew <= 0
        || static_cast<uintptr_t>(dos.e_lfanew) > UINTPTR_MAX - base)
        return false;
    IMAGE_NT_HEADERS64 nt{};
    if (!SafeRead(base + static_cast<uintptr_t>(dos.e_lfanew), nt)
        || nt.Signature != IMAGE_NT_SIGNATURE
        || nt.OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC
        || nt.OptionalHeader.SizeOfImage < sizeof(nt))
        return false;
    imageSize = nt.OptionalHeader.SizeOfImage;
    return true;
}

inline size_t FindUniqueBytes(const uint8_t* bytes, size_t count,
    const char* marker, size_t markerBytes) noexcept
{
    if (!bytes || !marker || markerBytes == 0 || markerBytes > count)
        return SIZE_MAX;
    size_t match = SIZE_MAX;
    for (size_t offset = 0; offset + markerBytes <= count; ++offset)
    {
        if (std::memcmp(bytes + offset, marker, markerBytes) != 0)
            continue;
        if (match != SIZE_MAX)
            return SIZE_MAX;
        match = offset;
    }
    return match;
}

inline uint64_t PackLuid(const LUID& luid) noexcept
{
    return static_cast<uint64_t>(luid.LowPart)
        | (static_cast<uint64_t>(static_cast<uint32_t>(luid.HighPart)) << 32);
}

// The pinned x64 MinHook build writes an E9 jump to an FF25 relay inside its own
// executable allocation. Returns that 14-byte relay only when `target` decodes to
// it and it jumps exactly to `detour`; null for any other route.
inline const unsigned char* MinHookRelay(const void* target, const void* detour) noexcept
{
    const auto* code = static_cast<const unsigned char*>(target);
    if (code[0] != 0xE9) return nullptr;
    int32_t displacement = 0;
    std::memcpy(&displacement, code + 1, sizeof(displacement));
    const auto* relay = code + 5 + displacement;
    constexpr size_t kRelayBytes = 14;
    MEMORY_BASIC_INFORMATION memory{};
    if (VirtualQuery(relay, &memory, sizeof(memory)) != sizeof(memory)
        || memory.State != MEM_COMMIT || memory.Type != MEM_PRIVATE
        || (memory.Protect & (PAGE_GUARD | PAGE_NOACCESS))
        || !(memory.Protect & (PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE))
        || memory.RegionSize < kRelayBytes
        || reinterpret_cast<uintptr_t>(relay) - reinterpret_cast<uintptr_t>(memory.BaseAddress)
            > memory.RegionSize - kRelayBytes) return nullptr;
    constexpr unsigned char indirect[] = {0xFF, 0x25, 0, 0, 0, 0};
    const void* destination = nullptr;
    std::memcpy(&destination, relay + sizeof(indirect), sizeof(destination));
    return !std::memcmp(relay, indirect, sizeof(indirect)) && destination == detour ? relay : nullptr;
}

}
