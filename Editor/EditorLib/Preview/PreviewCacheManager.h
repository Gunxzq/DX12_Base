#pragma once

#include <cstdint>
#include <string>
#include <filesystem>



/**
 * @brief 预览缓存管理器 — L1 磁盘缓存
 *
 * 负责缩略图 DDS 文件的读写和失效检测。
 * L2 内存缓存由 ThumbnailArray + LRU 策略在调用方实现。
 */
class PreviewCacheManager {
public:
    PreviewCacheManager() = default;
    ~PreviewCacheManager() = default;

    PreviewCacheManager(const PreviewCacheManager &) = delete;
    PreviewCacheManager &operator=(const PreviewCacheManager &) = delete;

    /// 设置缓存根目录（如 "Content/Cache/Thumbnails/"）
    void SetCacheDirectory(const std::string &path);

    /// 根据资产文件路径生成缓存键（相对路径哈希 → 文件名）
    std::string MakeCacheKey(const std::string &assetPath) const;

    /// 获取缓存文件的完整路径
    std::filesystem::path GetCacheFilePath(const std::string &cacheKey) const;

    /// 检查缓存是否存在且未过期
    bool IsCacheValid(const std::string &cacheKey, const std::filesystem::file_time_type &sourceTime) const;

    /// 读取 DDS 文件到内存缓冲区（调用方负责释放）
    /// @return 数据指针和大小，失败返回 {nullptr, 0}
    struct DDSData { void *data = nullptr; size_t size = 0; };
    DDSData ReadDDS(const std::string &cacheKey);

    /// 将 RGBA8 像素数据写入 DDS 文件
    /// @param width 纹理宽度
    /// @param height 纹理高度
    /// @param pixels RGBA8 像素数据（width * height * 4 bytes）
    /// @return 是否成功
    bool WriteDDS(const std::string &cacheKey, uint32_t width, uint32_t height, const void *pixels);

    /// 释放 DDS 读取返回的数据
    static void FreeDDSData(void *data);

    bool IsInitialized() const { return m_initialized; }

private:
    std::filesystem::path m_cacheDir;
    bool m_initialized = false;
};


