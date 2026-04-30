#include "System/Resource/Utils/PathUtils.h"
#include <algorithm>
#include <cctype>
#include <vector>

namespace DX12Engine {
namespace System {
namespace Resource {
namespace Utils {

namespace {
// 辅助函数：查找最后一个分隔符的位置
inline size_t FindLastSeparator(std::string_view path) {
    // 同时查找 / 和 \，兼容未标准化的输入
    size_t pos = path.find_last_of("/\\");
    return pos;
}

// 辅助函数：将字符转为小写
inline char ToLower(char c) { return static_cast<char>(std::tolower(static_cast<unsigned char>(c))); }
} // namespace

std::string PathUtils::Normalize(std::string_view path) {
    if (path.empty()) {
        return "";
    }

    // 预分配空间，最坏情况长度不变
    std::string result;
    result.reserve(path.size());

    std::vector<size_t> segmentStack;

    bool isAbsolute = false;
    if (!path.empty() && (path[0] == '/' || path[0] == '\\')) {
        isAbsolute = true;
        result += '/';
    } else if (path.size() > 1 && path[1] == ':') {
        // Windows 盘符处理，简单起见，保留盘符并视为绝对
        isAbsolute = true;
        result += path[0];
        result += ':';
        if (path.size() > 2 && (path[2] == '/' || path[2] == '\\')) {
            result += '/';
        }
    }

    size_t i = isAbsolute ? (result.size() > 1 ? 1 : 0) : 0;
    // 修正起始索引逻辑：如果上面加了盘符，i应该跳过盘符部分
    if (path.size() > 1 && path[1] == ':') {
        i = 2;
        if (i < path.size() && (path[i] == '/' || path[i] == '\\')) {
            i++;
        }
    } else if (isAbsolute) {
        i = 1; // 跳过前导 /
    } else {
        i = 0;
    }

    while (i < path.size()) {
        // 跳过分隔符
        if (path[i] == '/' || path[i] == '\\') {
            ++i;
            continue;
        }

        // 读取一个段落
        size_t start = i;
        while (i < path.size() && path[i] != '/' && path[i] != '\\') {
            ++i;
        }
        std::string_view segment = path.substr(start, i - start);

        if (segment == ".") {
            // 忽略当前目录
            continue;
        } else if (segment == "..") {
            // 回退一级
            if (!segmentStack.empty()) {
                // 找到上一个段落的起始位置，截断 result
                size_t prevStart = segmentStack.back();
                segmentStack.pop_back();
                result.resize(prevStart);

                if (!result.empty() && result.back() != '/') {
                }
            } else if (!isAbsolute) {

                if (!result.empty() && result.back() != '/') {
                    result += '/';
                }
                result += "..";
                segmentStack.push_back(result.size() - 2);
            }

        } else {
            // 普通段落
            if (!result.empty() && result.back() != '/') {
                result += '/';
            }
            segmentStack.push_back(result.size());
            result.append(segment.data(), segment.size());
        }
    }

    if (result.empty()) {
        return isAbsolute ? "/" : ".";
    }

    return result;
}

std::string_view PathUtils::GetExtension(std::string_view path) {
    // 先获取文件名部分，防止目录中有 .
    std::string_view fileName = GetFileName(path);
    size_t pos = fileName.find_last_of('.');
    if (pos == std::string_view::npos || pos == fileName.size() - 1) {
        return {};
    }

    return fileName.substr(pos + 1);
}

std::string_view PathUtils::GetFileName(std::string_view path) {
    size_t pos = FindLastSeparator(path);
    if (pos == std::string_view::npos) {
        return path;
    }
    return path.substr(pos + 1);
}

std::string PathUtils::GetStem(std::string_view path) {
    std::string_view fileName = GetFileName(path);
    size_t pos = fileName.find_last_of('.');
    if (pos == std::string_view::npos) {
        return std::string(fileName);
    }
    return std::string(fileName.substr(0, pos));
}

std::string_view PathUtils::GetDirectory(std::string_view path) {
    size_t pos = FindLastSeparator(path);
    if (pos == std::string_view::npos) {
        return {};
    }
    return path.substr(0, pos + 1);
}

std::string PathUtils::Combine(std::string_view base, std::string_view relative) {
    if (base.empty()) {
        return std::string(relative);
    }
    if (relative.empty()) {
        return std::string(base);
    }

    // 如果 relative 是绝对路径，直接返回
    if (IsAbsolute(relative)) {
        return std::string(relative);
    }

    std::string result;
    result.reserve(base.size() + relative.size() + 1);
    result.append(base.data(), base.size());

    // 确保 base 以分隔符结尾
    if (!result.empty() && result.back() != '/' && result.back() != '\\') {
        result += '/';
    }

    result.append(relative.data(), relative.size());

    return Normalize(result);
}

bool PathUtils::IsAbsolute(std::string_view path) {
    if (path.empty()) {
        return false;
    }

#ifdef _WIN32
    if (path.size() > 1 && path[1] == ':') {
        return true;
    }
    if (path[0] == '\\' || path[0] == '/') {
        return true;
    }
    return false;
#else
    return path[0] == '/';
#endif
}

} // namespace Utils
} // namespace Resource
} // namespace System
} // namespace DX12Engine