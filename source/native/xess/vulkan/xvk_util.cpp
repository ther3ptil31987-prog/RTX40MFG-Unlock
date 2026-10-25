#include "xvk_internal.h"
#include "single_module.h"
#include <bcrypt.h>
#include <cstdarg>
#include <cstdio>
#include <set>

namespace xvk {

namespace {
std::atomic<uint64_t> gLogLines{0};
} // namespace

// Joins RTXMFG's runtime log. The first 400 lines are kept, then one in 256 (each carries its running number).
void Log(const char* format, ...)
{
    const uint64_t line = ++gLogLines;
    if (line > 400 && line % 256) return;
    char text[1024];
    va_list args;
    va_start(args, format);
    vsnprintf(text, sizeof(text), format, args);
    va_end(args);
    wchar_t wide[1100];
    swprintf_s(wide, L"XVK #%llu %hs", line, text);
    single_module::Log(wide);
}

void Unimplemented(const char* what)
{
    static std::mutex mutex;
    static std::set<std::string> seen;
    {
        std::lock_guard lock(mutex);
        if (!seen.insert(what).second) return;
    }
    Log("unimplemented: %s", what);
}

HRESULT PrivateData::Get(REFGUID guid, UINT* size, void* data)
{
    if (!size) return E_INVALIDARG;
    std::lock_guard lock(mutex_);
    if (auto it = interfaces_.find(guid); it != interfaces_.end()) {
        if (!data) { *size = sizeof(IUnknown*); return S_OK; }
        if (*size < sizeof(IUnknown*)) return DXGI_ERROR_MORE_DATA;
        it->second->AddRef();
        memcpy(data, &it->second, sizeof(IUnknown*));
        *size = sizeof(IUnknown*);
        return S_OK;
    }
    auto it = data_.find(guid);
    if (it == data_.end()) { *size = 0; return DXGI_ERROR_NOT_FOUND; }
    if (!data) { *size = UINT(it->second.size()); return S_OK; }
    if (*size < it->second.size()) return DXGI_ERROR_MORE_DATA;
    memcpy(data, it->second.data(), it->second.size());
    *size = UINT(it->second.size());
    return S_OK;
}

HRESULT PrivateData::Set(REFGUID guid, UINT size, const void* data)
{
    std::lock_guard lock(mutex_);
    if (auto it = interfaces_.find(guid); it != interfaces_.end()) { it->second->Release(); interfaces_.erase(it); }
    if (!data || !size) { data_.erase(guid); return S_OK; }
    data_[guid].assign(static_cast<const uint8_t*>(data), static_cast<const uint8_t*>(data) + size);
    return S_OK;
}

HRESULT PrivateData::SetInterface(REFGUID guid, const IUnknown* object)
{
    std::lock_guard lock(mutex_);
    data_.erase(guid);
    if (auto it = interfaces_.find(guid); it != interfaces_.end()) { it->second->Release(); interfaces_.erase(it); }
    if (object) {
        auto* unknown = const_cast<IUnknown*>(object);
        unknown->AddRef();
        interfaces_[guid] = unknown;
    }
    return S_OK;
}

PrivateData::~PrivateData()
{
    for (auto& [guid, object] : interfaces_) object->Release();
}

std::string Sha256(const void* data, size_t size)
{
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    uint8_t digest[32]{};
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0) return {};
    bool ok = BCryptCreateHash(algorithm, &hash, nullptr, 0, nullptr, 0, 0) == 0 &&
              BCryptHashData(hash, static_cast<PUCHAR>(const_cast<void*>(data)), ULONG(size), 0) == 0 &&
              BCryptFinishHash(hash, digest, sizeof(digest), 0) == 0;
    if (hash) BCryptDestroyHash(hash);
    BCryptCloseAlgorithmProvider(algorithm, 0);
    if (!ok) return {};
    std::string text;
    char byte[3];
    for (uint8_t b : digest) { snprintf(byte, sizeof(byte), "%02x", b); text += byte; }
    return text;
}

FormatInfo Format(DXGI_FORMAT format, bool depthResource)
{
    const VkImageAspectFlags color = VK_IMAGE_ASPECT_COLOR_BIT;
    const VkImageAspectFlags depthStencil = VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
    switch (format) {
    case DXGI_FORMAT_R32G32B32A32_TYPELESS:
    case DXGI_FORMAT_R32G32B32A32_UINT: return {VK_FORMAT_R32G32B32A32_UINT, 16, color};
    case DXGI_FORMAT_R32G32B32A32_FLOAT: return {VK_FORMAT_R32G32B32A32_SFLOAT, 16, color};
    case DXGI_FORMAT_R32G32B32A32_SINT: return {VK_FORMAT_R32G32B32A32_SINT, 16, color};
    case DXGI_FORMAT_R32G32B32_TYPELESS:
    case DXGI_FORMAT_R32G32B32_UINT: return {VK_FORMAT_R32G32B32_UINT, 12, color};
    case DXGI_FORMAT_R32G32B32_FLOAT: return {VK_FORMAT_R32G32B32_SFLOAT, 12, color};
    case DXGI_FORMAT_R32G32B32_SINT: return {VK_FORMAT_R32G32B32_SINT, 12, color};
    case DXGI_FORMAT_R16G16B16A16_TYPELESS:
    case DXGI_FORMAT_R16G16B16A16_UINT: return {VK_FORMAT_R16G16B16A16_UINT, 8, color};
    case DXGI_FORMAT_R16G16B16A16_FLOAT: return {VK_FORMAT_R16G16B16A16_SFLOAT, 8, color};
    case DXGI_FORMAT_R16G16B16A16_UNORM: return {VK_FORMAT_R16G16B16A16_UNORM, 8, color};
    case DXGI_FORMAT_R16G16B16A16_SNORM: return {VK_FORMAT_R16G16B16A16_SNORM, 8, color};
    case DXGI_FORMAT_R16G16B16A16_SINT: return {VK_FORMAT_R16G16B16A16_SINT, 8, color};
    case DXGI_FORMAT_R32G32_TYPELESS:
    case DXGI_FORMAT_R32G32_UINT: return {VK_FORMAT_R32G32_UINT, 8, color};
    case DXGI_FORMAT_R32G32_FLOAT: return {VK_FORMAT_R32G32_SFLOAT, 8, color};
    case DXGI_FORMAT_R32G32_SINT: return {VK_FORMAT_R32G32_SINT, 8, color};
    case DXGI_FORMAT_R32G8X24_TYPELESS:
    case DXGI_FORMAT_D32_FLOAT_S8X24_UINT: return {VK_FORMAT_D32_SFLOAT_S8_UINT, 8, depthStencil, true};
    case DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS: return {VK_FORMAT_D32_SFLOAT_S8_UINT, 8, VK_IMAGE_ASPECT_DEPTH_BIT, true};
    case DXGI_FORMAT_X32_TYPELESS_G8X24_UINT: return {VK_FORMAT_D32_SFLOAT_S8_UINT, 8, VK_IMAGE_ASPECT_STENCIL_BIT, true};
    case DXGI_FORMAT_R10G10B10A2_TYPELESS:
    case DXGI_FORMAT_R10G10B10A2_UINT: return {VK_FORMAT_A2B10G10R10_UINT_PACK32, 4, color};
    case DXGI_FORMAT_R10G10B10A2_UNORM: return {VK_FORMAT_A2B10G10R10_UNORM_PACK32, 4, color};
    case DXGI_FORMAT_R11G11B10_FLOAT: return {VK_FORMAT_B10G11R11_UFLOAT_PACK32, 4, color};
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:
    case DXGI_FORMAT_R8G8B8A8_UNORM: return {VK_FORMAT_R8G8B8A8_UNORM, 4, color};
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: return {VK_FORMAT_R8G8B8A8_SRGB, 4, color};
    case DXGI_FORMAT_R8G8B8A8_UINT: return {VK_FORMAT_R8G8B8A8_UINT, 4, color};
    case DXGI_FORMAT_R8G8B8A8_SNORM: return {VK_FORMAT_R8G8B8A8_SNORM, 4, color};
    case DXGI_FORMAT_R8G8B8A8_SINT: return {VK_FORMAT_R8G8B8A8_SINT, 4, color};
    case DXGI_FORMAT_R16G16_TYPELESS:
    case DXGI_FORMAT_R16G16_UINT: return {VK_FORMAT_R16G16_UINT, 4, color};
    case DXGI_FORMAT_R16G16_FLOAT: return {VK_FORMAT_R16G16_SFLOAT, 4, color};
    case DXGI_FORMAT_R16G16_UNORM: return {VK_FORMAT_R16G16_UNORM, 4, color};
    case DXGI_FORMAT_R16G16_SNORM: return {VK_FORMAT_R16G16_SNORM, 4, color};
    case DXGI_FORMAT_R16G16_SINT: return {VK_FORMAT_R16G16_SINT, 4, color};
    case DXGI_FORMAT_R32_TYPELESS:
        return depthResource ? FormatInfo{VK_FORMAT_D32_SFLOAT, 4, VK_IMAGE_ASPECT_DEPTH_BIT, true} : FormatInfo{VK_FORMAT_R32_UINT, 4, color};
    case DXGI_FORMAT_D32_FLOAT: return {VK_FORMAT_D32_SFLOAT, 4, VK_IMAGE_ASPECT_DEPTH_BIT, true};
    case DXGI_FORMAT_R32_FLOAT:
        return depthResource ? FormatInfo{VK_FORMAT_D32_SFLOAT, 4, VK_IMAGE_ASPECT_DEPTH_BIT, true} : FormatInfo{VK_FORMAT_R32_SFLOAT, 4, color};
    case DXGI_FORMAT_R32_UINT: return {VK_FORMAT_R32_UINT, 4, color};
    case DXGI_FORMAT_R32_SINT: return {VK_FORMAT_R32_SINT, 4, color};
    case DXGI_FORMAT_R24G8_TYPELESS:
    case DXGI_FORMAT_D24_UNORM_S8_UINT: return {VK_FORMAT_D24_UNORM_S8_UINT, 4, depthStencil, true};
    case DXGI_FORMAT_R24_UNORM_X8_TYPELESS: return {VK_FORMAT_D24_UNORM_S8_UINT, 4, VK_IMAGE_ASPECT_DEPTH_BIT, true};
    case DXGI_FORMAT_X24_TYPELESS_G8_UINT: return {VK_FORMAT_D24_UNORM_S8_UINT, 4, VK_IMAGE_ASPECT_STENCIL_BIT, true};
    case DXGI_FORMAT_R8G8_TYPELESS:
    case DXGI_FORMAT_R8G8_UNORM: return {VK_FORMAT_R8G8_UNORM, 2, color};
    case DXGI_FORMAT_R8G8_UINT: return {VK_FORMAT_R8G8_UINT, 2, color};
    case DXGI_FORMAT_R8G8_SNORM: return {VK_FORMAT_R8G8_SNORM, 2, color};
    case DXGI_FORMAT_R8G8_SINT: return {VK_FORMAT_R8G8_SINT, 2, color};
    case DXGI_FORMAT_R16_TYPELESS:
        return depthResource ? FormatInfo{VK_FORMAT_D16_UNORM, 2, VK_IMAGE_ASPECT_DEPTH_BIT, true} : FormatInfo{VK_FORMAT_R16_UINT, 2, color};
    case DXGI_FORMAT_D16_UNORM: return {VK_FORMAT_D16_UNORM, 2, VK_IMAGE_ASPECT_DEPTH_BIT, true};
    case DXGI_FORMAT_R16_FLOAT: return {VK_FORMAT_R16_SFLOAT, 2, color};
    case DXGI_FORMAT_R16_UNORM:
        return depthResource ? FormatInfo{VK_FORMAT_D16_UNORM, 2, VK_IMAGE_ASPECT_DEPTH_BIT, true} : FormatInfo{VK_FORMAT_R16_UNORM, 2, color};
    case DXGI_FORMAT_R16_UINT: return {VK_FORMAT_R16_UINT, 2, color};
    case DXGI_FORMAT_R16_SNORM: return {VK_FORMAT_R16_SNORM, 2, color};
    case DXGI_FORMAT_R16_SINT: return {VK_FORMAT_R16_SINT, 2, color};
    case DXGI_FORMAT_R8_TYPELESS:
    case DXGI_FORMAT_R8_UNORM:
    case DXGI_FORMAT_A8_UNORM: return {VK_FORMAT_R8_UNORM, 1, color};
    case DXGI_FORMAT_R8_UINT: return {VK_FORMAT_R8_UINT, 1, color};
    case DXGI_FORMAT_R8_SNORM: return {VK_FORMAT_R8_SNORM, 1, color};
    case DXGI_FORMAT_R8_SINT: return {VK_FORMAT_R8_SINT, 1, color};
    case DXGI_FORMAT_R9G9B9E5_SHAREDEXP: return {VK_FORMAT_E5B9G9R9_UFLOAT_PACK32, 4, color};
    case DXGI_FORMAT_B8G8R8A8_TYPELESS:
    case DXGI_FORMAT_B8G8R8A8_UNORM:
    case DXGI_FORMAT_B8G8R8X8_TYPELESS:
    case DXGI_FORMAT_B8G8R8X8_UNORM: return {VK_FORMAT_B8G8R8A8_UNORM, 4, color};
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
    case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB: return {VK_FORMAT_B8G8R8A8_SRGB, 4, color};
    default: return {};
    }
}

} // namespace xvk
