#include "AssetLoader.h"
#include "Logger/Logger.h"
#include <filesystem>
#include <fstream>

namespace DX12Engine::Resource {

AssetLoader &AssetLoader::GetInstance() {
    static AssetLoader instance;
    return instance;
}

void AssetLoader::Initialize(const std::string &contentRoot) {
    m_contentRoot = contentRoot;
}

// 将相对路径解析为基于 ContentRoot 的完整路径
// 调用方传入的是相对 Content 的路径，如 "Textures/bricks.dds"
static std::wstring ResolveAssetPath(const std::wstring &path, const std::string &contentRoot) {
    if (std::filesystem::path(path).is_absolute())
        return path;
    return (std::filesystem::path(contentRoot) / path.c_str()).wstring();
}

bool AssetLoader::LoadTextureFromFile(const std::wstring &path, DDSTextureInfo &outInfo) {
    std::wstring fullPath = ResolveAssetPath(path, m_contentRoot);
    Logger::Logger::GetInstance()->Info("[AssetLoader] Loading texture: {}", std::string(fullPath.begin(), fullPath.end()));
    // 1. 打开文件
    std::ifstream file(fullPath, std::ios::binary | std::ios::ate);
    if (!file.is_open()) {
        ErrorReporter::Report("AssetLoader: Failed to open file");
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

// ========================================================================
// 地形加载
// ========================================================================

bool AssetLoader::LoadTerrainFromFile(const std::wstring &path, float width, float depth, float maxHeight,
                                      uint32_t segments, TerrainMeshData &outMesh) {
    std::wstring fullPath = ResolveAssetPath(path, m_contentRoot);
    std::ifstream file(fullPath, std::ios::binary | std::ios::ate);
    if (!file.is_open()) {
        ErrorReporter::Report("AssetLoader: Failed to open terrain file");
        return false;
    }

    size_t fileSize = file.tellg();
    file.seekg(0, std::ios::beg);

    std::vector<uint8_t> fileData(fileSize);
    file.read(reinterpret_cast<char *>(fileData.data()), fileSize);
    file.close();

    return LoadTerrainFromMemory(fileData.data(), fileSize, width, depth, maxHeight, segments, outMesh);
}

bool AssetLoader::LoadTerrainFromMemory(const uint8_t *data, size_t dataSize, float width, float depth, float maxHeight,
                                        uint32_t segments, TerrainMeshData &outMesh) {
    if (!data || dataSize == 0) {
        return false;
    }
    return TerrainLoader::LoadFromPNG(data, dataSize, width, depth, maxHeight, segments, outMesh);
}

} // namespace DX12Engine::Resource
