#pragma once

#include "Common/Common.h"
#include "Math/BoundingVolume.h"
#include "Renderer/Utils/GeometryGenerator.h"
#include "Resource/Struct/GeometryHandle.h"
#include "Resource/Core/GpuHandlePool.h"
#include <cstdint>

namespace DX12Engine {
namespace Resource {

// 规则网格几何体（XZ 平面，四边形网格拆分为三角形）
struct GridGeometry {
    // GPU 资源句柄
    GpuResourceHandle vertexBufferHandle;
    GpuResourceHandle indexBufferHandle;

    // 几何信息（与 TriangleMesh 一致）
    uint32_t vertexCount = 0;
    uint32_t indexCount = 0;
    uint32_t vertexStride = sizeof(GeometryGenerator::Vertex);
    DXGI_FORMAT indexFormat = DXGI_FORMAT_R32_UINT;
    D3D_PRIMITIVE_TOPOLOGY topology = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
    Math::BoundingVolumeVariant localBounds;
    bool isGpuReady = false;

    // 规则网格特有参数（最小必要）
    uint32_t widthSegments = 0; // X 轴分段数
    uint32_t depthSegments = 0; // Z 轴分段数

    // 派生属性（计算获得，不存储）
    uint32_t GetVertexCountX() const { return widthSegments + 1; }
    uint32_t GetVertexCountZ() const { return depthSegments + 1; }
    bool IsRegularGrid() const { return widthSegments > 0 && depthSegments > 0; }
};

} // namespace Resource
} // namespace DX12Engine