#include "HashUtils.h"

#include <cstring>
#include <iomanip>
#include <sstream>

namespace DX12Engine {

// FNV-1a 64-bit 常量
static constexpr uint64_t FNV_OFFSET_BASIS = 14695981039346656037ULL;
static constexpr uint64_t FNV_PRIME = 1099511628211ULL;

uint64_t HashUtils::CalculateMemoryHash(const void *data, size_t size) {
    if (!data || size == 0) {
        return 0;
    }

    uint64_t hash = FNV_OFFSET_BASIS;
    const uint8_t *bytes = static_cast<const uint8_t *>(data);

    for (size_t i = 0; i < size; ++i) {
        hash ^= bytes[i];
        hash *= FNV_PRIME;
    }

    return hash;
}

std::string HashUtils::CalculateFileHash(const void *data, size_t size) {
    if (!data || size == 0) {
        return "";
    }

    // 1. 计算 64位 FNV-1a 哈希
    uint64_t hashValue = CalculateMemoryHash(data, size);

    // 2. 将 64位整数转换为 16进制字符串 (16个字符)
    std::ostringstream oss;
    oss << std::hex << std::setw(16) << std::setfill('0') << hashValue;

    return oss.str();
}

bool HashUtils::VerifyFile(const void *data, size_t size, const std::string &expectedHash) {
    if (expectedHash.empty() || !data || size == 0) {
        return false;
    }

    std::string currentHash = CalculateFileHash(data, size);
    if (currentHash.empty()) {
        return false;
    }

    // 不区分大小写比较
#ifdef _WIN32
    return _stricmp(currentHash.c_str(), expectedHash.c_str()) == 0;
#else
    return strcasecmp(currentHash.c_str(), expectedHash.c_str()) == 0;
#endif
}

} // namespace DX12Engine