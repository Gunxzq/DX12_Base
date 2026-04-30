#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace DX12Engine {

class FileUtils {
public:
    // 禁止实例化，仅作为工具类使用
    FileUtils() = delete;
    ~FileUtils() = delete;

    /**
     * @brief 检查文件是否存在
     * @param path 文件路径
     * @return true 如果文件存在
     */
    static bool Exists(const std::string &path);

    /**
     * @brief 读取整个文件内容为二进制数据
     * @param path 文件路径
     * @param outData 输出缓冲区
     * @return true 如果读取成功
     */
    static bool ReadBinary(const std::string &path, std::vector<uint8_t> &outData);

    /**
     * @brief 读取整个文件内容为字符串
     * @param path 文件路径
     * @return 文件内容字符串，失败返回空字符串
     */
    static std::string ReadText(const std::string &path);

    /**
     * @brief 将二进制数据写入文件
     * @param path 文件路径
     * @param data 数据指针
     * @param size 数据大小
     * @return true 如果写入成功
     */
    static bool WriteBinary(const std::string &path, const void *data, size_t size);
};

} // namespace DX12Engine