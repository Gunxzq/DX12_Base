#pragma once

#include "Resource/AssetLoader/Loader/DDSLoader.h"
#include <string>
#include <vector>

namespace DX12Engine::Resource {

class AssetLoader {
public:
    static AssetLoader &GetInstance();

    // 从文件加载，返回解析后的纹理信息
    bool LoadTextureFromFile(const std::wstring &path, DDSTextureInfo &outInfo);

    // 从内存加载
    bool LoadTextureFromMemory(const uint8_t *data, size_t dataSize, DDSTextureInfo &outInfo);

private:
    // 根据扩展名获取解析器（未来扩展）
};

} // namespace DX12Engine::Resource