#include "XORCipher.h"
#include <algorithm>
#include <cstring>

namespace AssetTool {

XORCipher::XORCipher(uint32_t key)
    : m_key(key)
{
}

void XORCipher::DecryptBuffer(uint8_t *data, size_t size) const {
    // XOR 逐 int（4 字节块），不足 4 字节的尾部保持原样
    size_t alignedSize = size & ~3ULL; // 向下对齐到 4 字节
    for (size_t i = 0; i < alignedSize; i += 4) {
        uint32_t value;
        std::memcpy(&value, data + i, sizeof(uint32_t));
        value ^= m_key;
        std::memcpy(data + i, &value, sizeof(uint32_t));
    }
}

std::vector<uint8_t> XORCipher::DecryptCopy(const uint8_t *data, size_t size) const {
    std::vector<uint8_t> result(data, data + size);
    DecryptBuffer(result.data(), result.size());
    return result;
}

bool XORCipher::NeedsDecrypt(const std::string &extension) {
    // 小写比较
    std::string ext = extension;
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);

    return ext == ".hod" || ext == ".ani" || ext == ".mpd" ||
           ext == ".sdt" || ext == ".fx" || ext == ".dds";
}

} // namespace AssetTool
