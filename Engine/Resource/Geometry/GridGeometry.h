#pragma once

#include "Common/Common.h"
#include "Math/BoundingVolume.h"
#include "Renderer/Utils/GeometryGenerator.h"
#include "Resource/Core/GpuHandlePool.h"
#include "Resource/Geometry/GeometryBase.h"
#include "Resource/Struct/GeometryHandle.h"
#include <cstdint>

namespace DX12Engine {
namespace Resource {

// 规则网格几何体（XZ 平面，四边形网格拆分为三角形，水面用）
struct GridGeometry : GeometryBase {
    // 规则网格特有参数（最小必要）
    uint32_t widthSegments = 0; // X 轴分段数
    uint32_t depthSegments = 0; // Z 轴分段数

    // 派生属性（计算获得，不存储）
    uint32_t GetVertexCountX() const { return widthSegments + 1; }
    uint32_t GetVertexCountZ() const { return depthSegments + 1; }
    bool IsRegularGrid() const { return widthSegments > 0 && depthSegments > 0; }

    // 默认顶点步长（GeometryGenerator::Vertex）
    GridGeometry() { vertexStride = sizeof(GeometryGenerator::Vertex); }
};

} // namespace Resource
} // namespace DX12Engine