#pragma once
#include <cstdint>

namespace DX12Engine::Resource {

struct GeometryHandle {
    uint32_t index : 22;      // 最大支持 4,194,304 个几何体
    uint32_t generation : 10; // 最大 1024 次复用

    static constexpr GeometryHandle Invalid() { return {0x3FFFFF, 0}; }
    bool IsValid() const { return index != 0x3FFFFF; }
    operator uint32_t() const { return *reinterpret_cast<const uint32_t *>(this); }

    static GeometryHandle FromUint32(uint32_t val) {
        GeometryHandle h;
        *reinterpret_cast<uint32_t *>(&h) = val;
        return h;
    }
};

} // namespace DX12Engine::Resource