#pragma once
#include "Math/BoundingVolume.h"
#include <cstdint>

namespace DX12Engine::ECS {

// 拾取组件 — 标记实体可被拾取
struct PickingComponent {
    bool isPickable = true;
    int32_t priority = 0;
    uint32_t pickableBy = 0;
    bool enableHighlight = true;
    uint32_t highlightColor = 0xFFFFFFFF;
    bool editableInEditor = true;
    bool showBoundingBox = false;
};

// 静态实体组件（省计算 v3：烘焙矩阵 + 世界 AABB 缓存，未脏直接复用跳过计算）
struct StaticComponent {
    bool worldDirty = true;
    DirectX::XMFLOAT4X4 cachedWorld;
    DirectX::XMFLOAT4X4 cachedWorldInvTranspose;
    float cachedDistanceToCamera = 0.0f;
    // 省计算（v3）：世界 AABB 缓存，未脏直接复用（剔除跳过每帧 GetMatrix+Transform）
    Math::BoundingAABB cachedWorldBounds;
    bool hasCachedWorldBounds = false;
};

} // namespace DX12Engine::ECS
