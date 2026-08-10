#pragma once

#include "Renderer/Material/MaterialHandle.h"
#include <DirectXMath.h>
#include <cstdint>
#include <d3d12.h>

namespace DX12Engine::ECS {

// ========================================================================
// WaterComponent — 水体组件
//
// 每个水体实体携带此组件，配合 MeshComponent + TransparentTag 使用。
// wave params 直接从组件读取，由 WaterManager::CollectFromECS 收集后上传 GPU。
// waveParamIndex 和 objectCBAddress 由运行时填充，不序列化到场景 JSON。
// ========================================================================
struct WaterComponent {
    Resource::MaterialHandle materialHandle;

    // 波浪参数（从场景 JSON WaterDesc 同步，ECS 数据源）
    float amplitude = 0.5f;
    float frequency = 1.0f;
    float speed = 0.5f;
    float direction = 0.0f; // 风向（弧度）

    // 运行时状态（waveParamIndex 已废弃——CollectFromECS 按 ECS 顺序重建
    // m_waveParams，不再回写组件索引，见 WaterManager::CollectFromECS 注释）
    uint32_t waveParamIndex = UINT32_MAX;

    // objectCBAddress 已移除（§10.5 数据上传铁律：不建持久 CB，由 FrameSync 每帧上传）

    bool IsValid() const { return materialHandle.IsValid(); }
};

} // namespace DX12Engine::ECS
