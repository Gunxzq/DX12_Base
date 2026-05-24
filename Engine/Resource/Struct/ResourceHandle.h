// ResourceHandle.h
#pragma once
#include <cstdint>

namespace DX12Engine {

namespace Resource {

// CPU 端资源句柄：18位索引 + 10位世代号 + 4位池ID
struct CpuResourceHandle {
    uint32_t index : 18;      // 最大支持 262,144 个资源
    uint32_t generation : 10; // 最大 1024 次复用
    uint32_t poolId : 4;      // 池ID (0-15)

    static constexpr CpuResourceHandle Invalid() {
        return {0x3FFFF, 0, 0}; // Index 全1表示无效
    }

    bool IsValid() const { return index != 0x3FFFF; }

    // 用于在 Arena 中存储
    operator uint32_t() const { return *reinterpret_cast<const uint32_t *>(this); }

    static CpuResourceHandle FromUint32(uint32_t val) {
        CpuResourceHandle h;
        *reinterpret_cast<uint32_t *>(&h) = val;
        return h;
    }
};

// GPU 端资源句柄：22位索引 + 10位世代号
struct GpuResourceHandle {
    uint32_t index : 22;      // 最大支持 4,194,304 个 GPU 资源
    uint32_t generation : 10; // 最大 1024 次复用

    static constexpr GpuResourceHandle Invalid() {
        return {0x3FFFFF, 0}; // Index 全1表示无效
    }

    bool IsValid() const { return index != 0x3FFFFF; }

    operator uint32_t() const { return *reinterpret_cast<const uint32_t *>(this); }

    static GpuResourceHandle FromUint32(uint32_t val) {
        GpuResourceHandle h;
        *reinterpret_cast<uint32_t *>(&h) = val;
        return h;
    }
};

} // namespace Resource

} // namespace DX12Engine