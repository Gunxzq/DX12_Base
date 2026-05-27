#pragma once

#include <cstdint>

namespace DX12Engine::Resource {

struct TextureHandle {
    uint32_t index : 22;
    uint32_t generation : 10;

    static constexpr TextureHandle Invalid() { return {0x3FFFFF, 0}; }

    bool IsValid() const { return index != 0x3FFFFF; }

    operator uint32_t() const { return *reinterpret_cast<const uint32_t *>(this); }

    static TextureHandle FromUint32(uint32_t val) {
        TextureHandle h;
        *reinterpret_cast<uint32_t *>(&h) = val;
        return h;
    }
};

} // namespace DX12Engine::Resource