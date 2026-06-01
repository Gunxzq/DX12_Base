#pragma once

#include "Resource/Struct/GeometryHandle.h"
#include <DirectXMath.h>
#include <d3d12.h>

namespace DX12Engine::Renderer {

// ============================================================================
// 实体渲染器的渲染项 - 最终提交给 GPU 的数据
// ============================================================================
struct OpaqueRenderItem {
    // 几何体（GPU 直接使用）
    Resource::GeometryHandle geometryHandle;

    // 变换数据
    DirectX::XMMATRIX worldMatrix;
    D3D12_GPU_VIRTUAL_ADDRESS objectCBAddress; // 已上传的常量缓冲地址

    // 材质/纹理（GPU 直接使用）
    uint32_t materialIndex;
    D3D12_GPU_DESCRIPTOR_HANDLE textureSRV;

    bool IsValid() const { return geometryHandle.IsValid(); }
};

} // namespace DX12Engine::Renderer