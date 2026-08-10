#pragma once

#include "Common/Common.h"
#include "Math/BoundingVolume.h"
#include "Resource/Core/GpuHandlePool.h"
#include <cstdint>
#include <d3d12.h>
#include <vector>

namespace DX12Engine {
namespace Resource {

// SubMesh 信息（渲染时由 Builder 展开用，材质槽映射）
struct SubMeshInfo {
    uint32_t startIndex;
    uint32_t indexCount;
    int32_t startVertex;
};

// 几何体公共基类—所有图元都包含 VB/IB 地址 + 子网格信息（材质槽驱动）
struct GeometryBase {
    // GPU 资源句柄
    GpuResourceHandle vertexBufferHandle;
    GpuResourceHandle indexBufferHandle;

    // 几何信息
    uint32_t vertexCount = 0;
    uint32_t indexCount = 0;
    uint32_t vertexStride = 0;
    DXGI_FORMAT indexFormat = DXGI_FORMAT_R32_UINT;
    D3D_PRIMITIVE_TOPOLOGY topology = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;

    Math::BoundingVolumeVariant localBounds;

    // 子网格表（材质槽映射，所有图元都必须有）
    // 注册时自动兜底：空表 → 1 条子网格覆盖整个索引区间
    std::vector<SubMeshInfo> subMeshes;

    bool isGpuReady = false;
};

} // namespace Resource
} // namespace DX12Engine