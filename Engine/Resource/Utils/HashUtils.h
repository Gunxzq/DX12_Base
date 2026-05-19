#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace DX12Engine {

class HashUtils {
public:
    HashUtils() = delete;
    ~HashUtils() = delete;

    /**
     * @brief 计算内存块的快速哈希 (FNV-1a 64-bit)
     * @details 用于运行时资源去重、缓存键值生成。极速。
     * @param data 数据指针
     * @param size 数据大小
     * @return 64位哈希值
     */
    static uint64_t CalculateMemoryHash(const void *data, size_t size);

    /**
     * @brief 计算二进制数据的 FNV-1a 哈希字符串
     * @details 返回小写十六进制字符串 (16字符，基于64位哈希)。
     * @param data 数据指针
     * @param size 数据大小
     * @return FNV-1a 哈希字符串
     */
    static std::string CalculateFileHash(const void *data, size_t size);

    /**
     * @brief 验证二进制数据的哈希是否匹配预期值
     * @param data 数据指针
     * @param size 数据大小
     * @param expectedHash 预期的 FNV-1a 哈希字符串
     * @return true 如果匹配
     */
    static bool VerifyFile(const void *data, size_t size, const std::string &expectedHash);
};

} // namespace DX12Engine