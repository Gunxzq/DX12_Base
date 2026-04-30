// ResourceHandle.h
#pragma once
#include <cstdint>

namespace DX12Engine {
namespace System {
namespace Resource {

// 32位句柄：22位索引 + 10位世代号
struct ResourceHandle {
    uint32_t index : 22;      // 最大支持 4,194,304 个资源
    uint32_t generation : 10; // 最大 1024 次复用

    static constexpr ResourceHandle Invalid() {
        return {0x3FFFFF, 0}; // Index 全1表示无效
    }

    bool IsValid() const { return index != 0x3FFFFF; }

    // 用于在 Arena 中存储
    operator uint32_t() const { return *reinterpret_cast<const uint32_t *>(this); }

    static ResourceHandle FromUint32(uint32_t val) {
        ResourceHandle h;
        *reinterpret_cast<uint32_t *>(&h) = val;
        return h;
    }
};

} // namespace Resource
} // namespace System
} // namespace DX12Engine