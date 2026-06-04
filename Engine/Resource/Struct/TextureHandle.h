#pragma once

#include <cstdint>

namespace DX12Engine::Resource {

struct TextureHandle {
    uint32_t index : 22;
    uint32_t generation : 10;

    // 默认构造函数：初始化为 Invalid 状态，防止未初始化的 bit-field 被误判为有效
    TextureHandle() : index(0x3FFFFF), generation(0) {}

    static TextureHandle Invalid() {
        TextureHandle h;
        h.index = 0x3FFFFF;
        h.generation = 0;
        return h;
    }

    bool IsValid() const { return index != 0x3FFFFF; }

    operator uint32_t() const { return *reinterpret_cast<const uint32_t *>(this); }

    static TextureHandle FromUint32(uint32_t val) {
        TextureHandle h;
        *reinterpret_cast<uint32_t *>(&h) = val;
        return h;
    }
};

} // namespace DX12Engine::Resource