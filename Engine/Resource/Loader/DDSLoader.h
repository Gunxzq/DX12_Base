#pragma once

#include <cstdint>
#include <d3d12.h>
#include <string>
#include <vector>

namespace DX12Engine::Resource {

// ============================================================================
// DDS 纹理信息（解析后的结果，不包含 GPU 资源）
// ============================================================================
struct DDSTextureInfo {
    // 纹理属性
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t depth = 1;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    uint32_t mipLevels = 1;
    uint32_t arraySize = 1;
    bool isCubeMap = false;

    // 资源维度
    D3D12_RESOURCE_DIMENSION resourceDimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;

    // 像素数据（指向解析后的内存，不负责释放）
    const uint8_t *pixelData = nullptr;
    size_t pixelDataSize = 0;

    // 每个子资源的布局信息（用于 UpdateSubresources）
    std::vector<D3D12_SUBRESOURCE_DATA> subresources;

    bool IsValid() const { return format != DXGI_FORMAT_UNKNOWN && pixelData != nullptr; }
};

// ============================================================================
// DDS 加载器 - 只负责解析 DDS 文件，不涉及任何 GPU 操作
// ============================================================================
class DDSLoader {
public:
    static bool LoadFromFile(const std::wstring &path, DDSTextureInfo &outInfo);
    static bool LoadFromMemory(const uint8_t *data, size_t dataSize, DDSTextureInfo &outInfo);

private:
    static bool ParseDDS(const uint8_t *fileData, size_t fileSize, DDSTextureInfo &outInfo);
    static DXGI_FORMAT GetDXGIFormat(const void *pixelFormat);
    static void GetSurfaceInfo(size_t width, size_t height, DXGI_FORMAT format, size_t &outNumBytes,
                               size_t &outRowBytes);
    static bool FillSubresourceData(const uint8_t *bitData, size_t bitSize, const DDSTextureInfo &info, size_t maxsize,
                                    std::vector<D3D12_SUBRESOURCE_DATA> &outSubresources, uint32_t &outSkipMip);
};

} // namespace DX12Engine::Resource