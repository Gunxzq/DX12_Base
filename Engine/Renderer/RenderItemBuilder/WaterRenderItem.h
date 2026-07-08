#pragma once

#include "Resource/Struct/GeometryHandle.h"
#include <DirectXMath.h>
#include <d3d12.h>

namespace DX12Engine::Renderer {

// ============================================================================
// WaterRenderItem — 水体渲染项
//
// 由 WaterRenderItemBuilder 在 PreRender 阶段构建。
// objectCBAddress 来自 WaterComponent（持久 UPLOAD 堆，ConstructEntity 分配）。
// waveParamIndex 引用 WaterManager 中的波浪参数。
// ============================================================================
struct WaterRenderItem {
    Resource::GeometryHandle geometryHandle;
    D3D12_GPU_VIRTUAL_ADDRESS objectCBAddress = 0; // 持久 UPLOAD 堆地址
    DirectX::XMMATRIX worldMatrix;
    uint32_t waveParamIndex = UINT32_MAX;
    uint32_t materialIndex = 0;
    float depth = 0.0f;

    bool IsValid() const { return geometryHandle.IsValid(); }
};

} // namespace DX12Engine::Renderer
