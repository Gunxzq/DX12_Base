#include "FileUtils.h"

#include <filesystem>
#include <fstream>
#include <system_error>

namespace DX12Engine {

bool FileUtils::Exists(const std::string &path) {
    std::error_code ec;
    // 使用 error_code 版本避免异常抛出，提高性能
    return std::filesystem::exists(path, ec) && !ec;
}

bool FileUtils::ReadBinary(const std::string &path, std::vector<uint8_t> &outData) {
    // 以二进制模式打开，并定位到末尾获取大小
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file.is_open()) {
        return false;
    }

    // 获取文件大小
    auto fileSize = file.tellg();
    if (fileSize <= 0) {
        return false;
    }

    // 重置指针到开头
    file.seekg(0, std::ios::beg);

    // 预分配内存
    outData.resize(static_cast<size_t>(fileSize));

    // 读取数据
    if (!file.read(reinterpret_cast<char *>(outData.data()), fileSize)) {
        outData.clear();
        return false;
    }

    return true;
}

std::string FileUtils::ReadText(const std::string &path) {
    std::ifstream file(path, std::ios::in);
    if (!file.is_open()) {
        return "";
    }

    // 利用迭代器一次性读取所有内容
    return std::string((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
}

bool FileUtils::WriteBinary(const std::string &path, const void *data, size_t size) {
    // 确保父目录存在（可选，根据需求决定）
    std::filesystem::path fsPath(path);
    if (fsPath.has_parent_path()) {
        std::error_code ec;
        std::filesystem::create_directories(fsPath.parent_path(), ec);
        if (ec) {
            return false;
        }
    }

    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file.is_open()) {
        return false;
    }

    file.write(reinterpret_cast<const char *>(data), static_cast<std::streamsize>(size));

    // 检查是否写入成功
    return file.good();
}

} // namespace DX12Engine