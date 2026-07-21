#include "PreviewCacheManager.h"
#include "Common/Common.h"
#include <cstring>
#include <fstream>



// ========================================================================
// DDS 文件结构（简化版，仅支持 RGBA8  uncompressed）
// ========================================================================
#pragma pack(push, 1)
struct DDS_PIXELFORMAT {
    uint32_t size = 32;
    uint32_t flags = 0x41; // DDPF_RGB | DDPF_ALPHAPIXELS
    uint32_t fourCC = 0;
    uint32_t RGBBitCount = 32;
    uint32_t RBitMask = 0x00FF0000;
    uint32_t GBitMask = 0x0000FF00;
    uint32_t BBitMask = 0x000000FF;
    uint32_t ABitMask = 0xFF000000;
};

struct DDS_HEADER {
    uint32_t size = 124;
    uint32_t flags = 0x100F; // DDSD_CAPS | DDSD_HEIGHT | DDSD_WIDTH | DDSD_PITCH | DDSD_PIXELFORMAT | DDSD_MIPMAPCOUNT
    uint32_t height;
    uint32_t width;
    uint32_t pitchOrLinearSize;
    uint32_t depth = 0;
    uint32_t mipMapCount = 1;
    uint32_t reserved1[11] = {};
    DDS_PIXELFORMAT pixelFormat;
    uint32_t caps = 0x1000; // DDSCAPS_TEXTURE
    uint32_t caps2 = 0;
    uint32_t caps3 = 0;
    uint32_t caps4 = 0;
    uint32_t reserved2 = 0;
};
#pragma pack(pop)

static const uint32_t DDS_MAGIC = 0x20534444; // "DDS "

// ========================================================================
// 实现
// ========================================================================

void PreviewCacheManager::SetCacheDirectory(const std::string &path) {
    m_cacheDir = std::filesystem::absolute(path);
    std::filesystem::create_directories(m_cacheDir);
    m_initialized = true;
}

std::string PreviewCacheManager::MakeCacheKey(const std::string &assetPath) const {
    // 用资产路径的哈希作为缓存键
    std::hash<std::string> hasher;
    return std::to_string(hasher(assetPath));
}

std::filesystem::path PreviewCacheManager::GetCacheFilePath(const std::string &cacheKey) const {
    return m_cacheDir / (cacheKey + ".dds");
}

bool PreviewCacheManager::IsCacheValid(const std::string &cacheKey,
                                       const std::filesystem::file_time_type &sourceTime) const {
    auto cachePath = GetCacheFilePath(cacheKey);
    if (!std::filesystem::exists(cachePath))
        return false;
    // 缓存文件必须比源文件新
    auto cacheTime = std::filesystem::last_write_time(cachePath);
    return cacheTime >= sourceTime;
}

PreviewCacheManager::DDSData PreviewCacheManager::ReadDDS(const std::string &cacheKey) {
    DDSData result = {nullptr, 0};
    auto path = GetCacheFilePath(cacheKey);
    if (!std::filesystem::exists(path))
        return result;

    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file.is_open())
        return result;

    size_t fileSize = static_cast<size_t>(file.tellg());
    file.seekg(0);

    auto *buffer = new uint8_t[fileSize];
    file.read(reinterpret_cast<char *>(buffer), fileSize);
    file.close();

    result.data = buffer;
    result.size = fileSize;
    return result;
}

bool PreviewCacheManager::WriteDDS(const std::string &cacheKey, uint32_t width, uint32_t height, const void *pixels) {
    if (!m_initialized || !pixels || width == 0 || height == 0)
        return false;

    auto path = GetCacheFilePath(cacheKey);

    DDS_HEADER header;
    header.width = width;
    header.height = height;
    header.pitchOrLinearSize = width * 4; // RGBA8 row pitch

    uint32_t pixelDataSize = width * height * 4;

    std::ofstream file(path, std::ios::binary);
    if (!file.is_open())
        return false;

    file.write(reinterpret_cast<const char *>(&DDS_MAGIC), sizeof(DDS_MAGIC));
    file.write(reinterpret_cast<const char *>(&header), sizeof(header));
    file.write(reinterpret_cast<const char *>(pixels), pixelDataSize);

    file.close();
    return true;
}

void PreviewCacheManager::FreeDDSData(void *data) {
    delete[] static_cast<uint8_t *>(data);
}


