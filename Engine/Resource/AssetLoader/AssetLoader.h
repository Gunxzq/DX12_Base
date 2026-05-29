#pragma once

#include "Resource/AssetLoader/Loader/DDSLoader.h"
#include "Resource/AssetLoader/Loader/TerrainLoader.h"
#include <string>
#include <vector>

namespace DX12Engine::Resource {

class AssetLoader {
public:
    static AssetLoader &GetInstance();

    // 从文件加载，返回解析后的纹理信息
    bool LoadTextureFromFile(const std::wstring &path, DDSTextureInfo &outInfo);

    // 从内存加载纹理，返回解析后的纹理信息
    bool LoadTextureFromMemory(const uint8_t *data, size_t dataSize, DDSTextureInfo &outInfo);

    // 地形加载
    bool LoadTerrainFromFile(const std::wstring &path, float width, float depth, float maxHeight, uint32_t segments,
                             TerrainMeshData &outMesh);

    bool LoadTerrainFromMemory(const uint8_t *data, size_t dataSize, float width, float depth, float maxHeight,
                               uint32_t segments, TerrainMeshData &outMesh);

private:
    // 根据扩展名获取解析器（未来扩展）
};

} // namespace DX12Engine::Resource