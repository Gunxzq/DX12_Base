#pragma once

#include <cstdint>
#include <d3d12.h>
#include <string>
#include <vector>

namespace DX12Engine::Resource {

struct DDSTextureInfo {
    D3D12_RESOURCE_DESC desc = {};
    std::vector<D3D12_SUBRESOURCE_DATA> subresources;
    std::vector<uint8_t> pixelData;
    bool isCubeMap = false;
};

class DDSLoader {
public:
    static bool LoadFromMemory(const uint8_t *data, size_t dataSize, DDSTextureInfo &outInfo);

    static bool ParseDDS(const uint8_t *fileData, size_t fileSize, DDSTextureInfo &outInfo);
};

} // namespace DX12Engine::Resource