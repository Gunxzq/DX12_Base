#pragma once

#include <cstdint>

namespace DX12Engine::Resource {
// 渲染目标句柄结构
struct RenderTargetHandle {
    uint32_t poolIndex : 12;
    uint32_t generation : 8;
    uint32_t rtvSlot : 12;

    static constexpr RenderTargetHandle Invalid() {
        RenderTargetHandle h;
        h.poolIndex = 0xFFF;
        h.generation = 0;
        h.rtvSlot = 0xFFF;
        return h;
    }

    bool IsValid() const { return poolIndex != 0xFFF && rtvSlot != 0xFFF; }

    operator uint32_t() const { return *reinterpret_cast<const uint32_t *>(this); }
};

// 深度模板句柄结构
struct DepthStencilHandle {
    uint32_t poolIndex : 12;
    uint32_t generation : 8;
    uint32_t dsvSlot : 12;

    static constexpr DepthStencilHandle Invalid() {
        DepthStencilHandle h;
        h.poolIndex = 0xFFF;
        h.generation = 0;
        h.dsvSlot = 0xFFF;
        return h;
    }

    bool IsValid() const { return poolIndex != 0xFFF && dsvSlot != 0xFFF; }

    operator uint32_t() const { return *reinterpret_cast<const uint32_t *>(this); }
};
} // namespace DX12Engine::Resource