#pragma once
#include <cstdint>

namespace DX12Engine::Resource {

// ========================================================================
// ResourceType — 资产/资源类型标识
// 编码在事件 payload 高位 (32 bits)，用于 ResourceReadyEvent 等通用事件
// 的分发路由
// ========================================================================

enum class ResourceType : uint32_t {
    Terrain = 0,
    // Mesh    = 1,   // 后续扩展
    // Texture = 2,
    // Material = 3,
    // Scene   = 4,
};

} // namespace DX12Engine::Resource
