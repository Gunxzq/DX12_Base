#include "AssetLoader.h"
#include <fstream>

namespace DX12Engine::Resource {

AssetLoader &AssetLoader::GetInstance() {
    static AssetLoader instance;
    return instance;
}

bool AssetLoader::LoadTextureFromFile(const std::wstring &path, DDSTextureInfo &outInfo) {
    // 1. 打开文件
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file.is_open()) {
        OutputDebugStringW(L"[ERROR] AssetLoader: Failed to open file\n");
        return false;
    }

    // 2. 读取二进制数据
    size_t fileSize = file.tellg();
    file.seekg(0, std::ios::beg);

    std::vector<uint8_t> fileData(fileSize);
    file.read(reinterpret_cast<char *>(fileData.data()), fileSize);
    file.close();

    // 3. 调用 DDSLoader 解析
    return DDSLoader::LoadFromMemory(fileData.data(), fileSize, outInfo);
}

bool AssetLoader::LoadTextureFromMemory(const uint8_t *data, size_t dataSize, DDSTextureInfo &outInfo) {
    if (!data || dataSize == 0) {
        return false;
    }
    return DDSLoader::LoadFromMemory(data, dataSize, outInfo);
}

} // namespace DX12Engine::Resource