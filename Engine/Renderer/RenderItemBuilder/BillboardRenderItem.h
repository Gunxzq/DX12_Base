#pragma once

#include "ECS/Core/Components.h"
#include "Resource/Struct/GeometryHandle.h"
#include <d3d12.h>

namespace DX12Engine::Renderer {

// ============================================================================
// 公告牌渲染项 - 用于几何着色器公告牌渲染
// ============================================================================
struct BillboardRenderItem {

    D3D12_GPU_VIRTUAL_ADDRESS instanceBufferAddress = 0;
    uint32_t instanceCount = 0;

    D3D12_GPU_DESCRIPTOR_HANDLE textureSRV = {};

    bool IsValid() const { return instanceBufferAddress != 0 && instanceCount > 0; }
};

} // namespace DX12Engine::Renderer