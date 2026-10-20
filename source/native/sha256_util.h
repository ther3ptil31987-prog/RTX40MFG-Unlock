#pragma once
// SHA-256 through Windows CNG (bcrypt.dll; the module delay-loads it). Shared by the provider,
// kernel, payload and shader-cache checks; each caller keeps its own size bounds and failure policy.
#include <Windows.h>
#include <bcrypt.h>
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <string>

namespace sha256_util
{
using Digest = std::array<uint8_t, 32>;

class Hasher
{
public:
    Hasher() noexcept
    {
        okay_ = BCryptOpenAlgorithmProvider(&algorithm_, BCRYPT_SHA256_ALGORITHM, nullptr, 0) >= 0
            && BCryptCreateHash(algorithm_, &hash_, nullptr, 0, nullptr, 0, 0) >= 0;
    }
    ~Hasher()
    {
        if (hash_) BCryptDestroyHash(hash_);
        if (algorithm_) BCryptCloseAlgorithmProvider(algorithm_, 0);
    }
    Hasher(const Hasher&) = delete;
    Hasher& operator=(const Hasher&) = delete;

    // Hashes in chunks of at most 1 MiB, so any size_t count fits CNG's ULONG lengths.
    bool Update(const void* data, size_t count) noexcept
    {
        const auto* bytes = static_cast<const uint8_t*>(data);
        for (size_t at = 0; okay_ && at < count;)
        {
            const auto chunk = static_cast<ULONG>(std::min<size_t>(count - at, size_t{1} << 20));
            okay_ = BCryptHashData(hash_, const_cast<PUCHAR>(bytes + at), chunk, 0) >= 0;
            at += chunk;
        }
        return okay_;
    }
    // False (and an all-zero digest) on any CNG failure.
    bool Finish(Digest& digest) noexcept
    {
        digest = {};
        okay_ = okay_ && BCryptFinishHash(hash_, digest.data(), static_cast<ULONG>(digest.size()), 0) >= 0;
        if (!okay_) digest = {};
        return okay_;
    }

private:
    BCRYPT_ALG_HANDLE algorithm_ = nullptr;
    BCRYPT_HASH_HANDLE hash_ = nullptr;
    bool okay_ = false;
};

inline bool Compute(const void* data, size_t count, Digest& digest) noexcept
{
    Hasher hasher;
    return hasher.Update(data, count) && hasher.Finish(digest);
}

// True when 1..maxBytes bytes hash to `expected`, exactly 64 uppercase hexadecimal digits.
inline bool EqualsUpperHex(const uint8_t* bytes, size_t count, const char* expected, size_t maxBytes) noexcept
{
    if (!bytes || count == 0 || count > maxBytes || !expected || std::strlen(expected) != 64) return false;
    Digest digest{};
    if (!Compute(bytes, count, digest)) return false;
    constexpr char digits[] = "0123456789ABCDEF";
    for (size_t i = 0; i < digest.size(); ++i)
        if (expected[i * 2] != digits[digest[i] >> 4] || expected[i * 2 + 1] != digits[digest[i] & 15]) return false;
    return true;
}

inline std::string LowerHex(const Digest& digest)
{
    constexpr char digits[] = "0123456789abcdef";
    std::string text;
    text.reserve(digest.size() * 2);
    for (const uint8_t value : digest)
    {
        text += digits[value >> 4];
        text += digits[value & 15];
    }
    return text;
}
}
