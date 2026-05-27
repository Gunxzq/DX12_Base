#pragma once

#include <cstdint>
#include <d3d12.h>
#include <string>
#include <vector>

namespace DX12Engine::Resource {

struct DDSTextureInfo {
    D3D12_RESOURCE_DESC desc = {};
    std::vector<D3D12_SUBRESOURCE_DATA> subresources;
    const uint8_t *pixelData = nullptr;
    size_t pixelDataSize = 0;
    bool isCubeMap = false;
};

class DDSLoader {
public:
    static bool LoadFromFile(const std::wstring &path, DDSTextureInfo &outInfo);
    static bool LoadFromMemory(const uint8_t *data, size_t dataSize, DDSTextureInfo &outInfo);

private:
    static bool ParseDDS(const uint8_t *fileData, size_t fileSize, DDSTextureInfo &outInfo);
    static bool FillSubresourceData(const uint8_t *bitData, size_t bitSize, const DDSTextureInfo &info, size_t maxsize,
                                    std::vector<D3D12_SUBRESOURCE_DATA> &outSubresources, uint32_t &outSkipMip);
};

} // namespace DX12Engine::Resource