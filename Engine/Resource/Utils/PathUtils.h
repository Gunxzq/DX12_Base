#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace DX12Engine {
namespace System {
namespace Resource {
namespace Utils {

/**
 * @brief 轻量级路径工具类
 *
 * 专注于高性能的路径标准化和拆分，避免不必要的内存分配。
 * 内部统一使用 '/' 作为分隔符。
 */
class PathUtils {
public:
    /**
     * @brief 标准化路径
     *
     * 1. 统一分隔符为 '/'
     * 2. 移除连续的 '/'
     * 3. 解析 '.' (当前目录) 和 '..' (上级目录)
     *
     * @param path 原始路径
     * @return 标准化后的路径 (始终以 '/' 分隔，除非为空)
     */
    static std::string Normalize(std::string_view path);

    /**
     * @brief 获取文件扩展名
     *
     * @param path 文件路径
     * @return 小写化的扩展名 (不含点)，例如 "png"。如果无扩展名返回空视图。
     */
    static std::string_view GetExtension(std::string_view path);

    /**
     * @brief 获取文件名 (含扩展名)
     *
     * @param path 文件路径
     * @return 文件名视图
     */
    static std::string_view GetFileName(std::string_view path);

    /**
     * @brief 获取不带扩展名的文件名
     *
     * @param path 文件路径
     * @return 纯文件名视图
     */
    static std::string GetStem(std::string_view path);

    /**
     * @brief 获取目录路径
     *
     * @param path 文件路径
     * @return 目录部分 (以 '/' 结尾，除非是根目录或无目录)
     */
    static std::string_view GetDirectory(std::string_view path);

    /**
     * @brief 组合路径
     *
     * @param base 基础路径
     * @param relative 相对路径
     * @return 组合并标准化后的完整路径
     */
    static std::string Combine(std::string_view base, std::string_view relative);

    /**
     * @brief 判断是否为绝对路径
     */
    static bool IsAbsolute(std::string_view path);
};

} // namespace Utils
} // namespace Resource
} // namespace System
} // namespace DX12Engine