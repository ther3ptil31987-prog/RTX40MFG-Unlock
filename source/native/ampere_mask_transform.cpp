#include "ampere_mask_transform.h"
#include "sha256_util.h"
#include <Windows.h>
#include <bcrypt.h>
#include <array>
#include <cstring>

namespace ampere_mask_transform
{
namespace
{
bool Sha256Equals(const uint8_t* bytes, size_t count, const char* expected) noexcept
{
    return sha256_util::EqualsUpperHex(bytes, count, expected, 64u * 1024u * 1024u);
}

size_t FindUniqueBytes(const uint8_t* bytes, size_t count,
    const char* marker, size_t markerBytes) noexcept
{
    if (!bytes || !marker || markerBytes == 0 || markerBytes > count)
        return SIZE_MAX;
    size_t found = SIZE_MAX;
    for (size_t offset = 0; offset <= count - markerBytes; ++offset)
    {
        if (std::memcmp(bytes + offset, marker, markerBytes) != 0)
            continue;
        if (found != SIZE_MAX)
            return SIZE_MAX;
        found = offset;
    }
    return found;
}

#include "output_pull_mask_transform.inl"
}

bool Apply(const uint8_t* sourceFatbin, size_t sourceFatbinBytes,
    uint8_t* ptx, size_t& ptxBytes, size_t capacity) noexcept
{
    const OutputPullMaskProfile* selected = nullptr;
    for (const auto* profile : {&kOutputPullMaskLegacy, &kOutputPullMask3109})
    {
        if (sourceFatbinBytes == profile->sourceBytes
            && Sha256Equals(sourceFatbin, sourceFatbinBytes, profile->sourceSha256))
        {
            if (selected)
                return false;
            selected = profile;
        }
    }
    if (!selected || ptxBytes != selected->sm89RawBytes)
        return false;
    uint32_t changedBytes = 0;
    if (!RewriteOutputPullMaskPtx(ptx, capacity, *selected, false, changedBytes))
        return false;
    ptxBytes = changedBytes;
    return true;
}
}
