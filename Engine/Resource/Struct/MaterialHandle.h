#pragma once

#include <cstdint>

namespace DX12Engine {

namespace Resource {
struct MaterialHandle {
    uint32_t index : 22;      // 最大支持 4,194,304 个材质
    uint32_t generation : 10; // 世代号

    static constexpr MaterialHandle Invalid() { return {0x3FFFFF, 0}; }
    bool IsValid() const { return index != 0x3FFFFF; }
    operator uint32_t() const { return *reinterpret_cast<const uint32_t *>(this); }

    // FromUint32
    static MaterialHandle FromUint32(uint32_t val) {
        MaterialHandle handle;
        *reinterpret_cast<uint32_t *>(&handle) = val;
        return handle;
    }

    // GetHash
    uint32_t GetHash() const { return index; }
};
} // namespace Resource
} // namespace DX12Engine