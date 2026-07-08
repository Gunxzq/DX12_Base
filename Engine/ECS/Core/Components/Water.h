#pragma once

#include "Renderer/Material/MaterialHandle.h"
#include <cstdint>
#include <d3d12.h>

namespace DX12Engine::ECS {

// ========================================================================
// WaterComponent — 水体组件
//
// 每个水体实体携带此组件，配合 MeshComponent + TransparentTag 使用。
// waveParamIndex 引用 WaterManager 中的波浪参数。
// objectCBAddress 由 ConstructEntity 分配持久 UPLOAD 堆。
// ========================================================================
struct WaterComponent {
    Resource::MaterialHandle materialHandle;
    uint32_t waveParamIndex = UINT32_MAX;
    D3D12_GPU_VIRTUAL_ADDRESS objectCBAddress = 0;

    bool IsValid() const { return materialHandle.IsValid() && waveParamIndex != UINT32_MAX && objectCBAddress != 0; }
};

} // namespace DX12Engine::ECS
