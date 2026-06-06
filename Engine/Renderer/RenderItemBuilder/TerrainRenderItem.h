#pragma once

#include "Resource/Struct/GeometryHandle.h"
#include <d3d12.h>

namespace DX12Engine::Renderer {

// ============================================================================
// 地形渲染项 - 支持曲面细分的地形专用渲染数据
//   地形不需要实例化，每个地形块单独绘制
// ============================================================================
struct TerrainRenderItem {
    // 几何体句柄（指向 PatchMesh）
    Resource::GeometryHandle geometryHandle;

    // 每物体常量缓冲区 GPU 地址（cbPerObject）
    D3D12_GPU_VIRTUAL_ADDRESS objectCBAddress;

    // 纹理 SRV
    D3D12_GPU_DESCRIPTOR_HANDLE heightMapSRV; // 高度图（用于顶点置换，必需）
    D3D12_GPU_DESCRIPTOR_HANDLE albedoSRV;    // 漫反射纹理（可选，用于像素着色）
    D3D12_GPU_DESCRIPTOR_HANDLE normalSRV;    // 法线贴图（可选）

    // 曲面细分参数（LOD 控制）
    float heightScale = 1.0f;               // 高度缩放
    float heightOffset = 0.0f;              // 高度偏移
    float tessellationFactor = 8.0f;        // 近距离最大细分因子
    float tessellationDistanceMin = 5.0f;   // 近距离边界
    float tessellationDistanceMax = 80.0f;  // 远距离边界（超出后不细分）

    // 材质参数
    uint32_t materialIndex;

    bool IsValid() const { return geometryHandle.IsValid() && heightMapSRV.ptr != 0; }
};

} // namespace DX12Engine::Renderer