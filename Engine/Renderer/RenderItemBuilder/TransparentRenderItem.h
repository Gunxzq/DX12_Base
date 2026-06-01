#pragma once

#include "Resource/Struct/GeometryHandle.h"
#include <DirectXMath.h>
#include <d3d12.h>

namespace DX12Engine::Renderer {

// ============================================================================
// 透明渲染项 - 最终提交给 GPU 的数据，包含深度用于远到近排序
// ============================================================================
struct TransparentRenderItem {
    // 几何体（GPU 直接使用）
    Resource::GeometryHandle geometryHandle;

    // 变换数据
    DirectX::XMMATRIX worldMatrix;
    D3D12_GPU_VIRTUAL_ADDRESS objectCBAddress; // 已上传的常量缓冲地址

    // 材质/纹理（GPU 直接使用）
    uint32_t materialIndex;
    D3D12_GPU_DESCRIPTOR_HANDLE textureSRV;

    // 深度值（到相机的距离），用于远到近排序
    float depth = 0.0f;

    bool IsValid() const { return geometryHandle.IsValid(); }
};

} // namespace DX12Engine::Renderer
