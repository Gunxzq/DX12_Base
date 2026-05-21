// ResourceHandle.h
#pragma once
#include <cstdint>

namespace DX12Engine {

namespace Resource {

// 32位句柄：18位索引 + 10位世代号 + 4位池ID
struct ResourceHandle {
    uint32_t index : 18;      // 最大支持 262,144 个资源
    uint32_t generation : 10; // 最大 1024 次复用
    uint32_t poolId : 4;      // 池ID (0-15)

    static constexpr ResourceHandle Invalid() {
        return {0x3FFFF, 0, 0}; // Index 全1表示无效
    }

    bool IsValid() const { return index != 0x3FFFF; }

    // 用于在 Arena 中存储
    operator uint32_t() const { return *reinterpret_cast<const uint32_t *>(this); }

    static ResourceHandle FromUint32(uint32_t val) {
        ResourceHandle h;
        *reinterpret_cast<uint32_t *>(&h) = val;
        return h;
    }
};

} // namespace Resource

} // namespace DX12Engine