#pragma once
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

// 静态实体持久化组件（暂未启用）
struct StaticComponent {
    D3D12_GPU_VIRTUAL_ADDRESS persistentCBAddress = 0;
    D3D12_GPU_VIRTUAL_ADDRESS persistentInstanceAddress = 0;
    uint32_t persistentInstanceSize = 0;
    uint32_t batchInstanceIndex = UINT32_MAX;
    bool worldDirty = true;
    DirectX::XMFLOAT4X4 cachedWorld;
    DirectX::XMFLOAT4X4 cachedWorldInvTranspose;
    float cachedDistanceToCamera = 0.0f;
};

} // namespace DX12Engine::ECS
