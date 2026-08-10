#pragma once

#include "Math/BoundingVolume.h"
#include "Resource/Core/GpuHandlePool.h"
#include "Resource/Geometry/GeometryBase.h"
#include <DirectXMath.h>
#include <d3d12.h>

namespace DX12Engine::Resource {

enum class PatchType : uint8_t {
    Quad,    // 四边形面片，4个控制点
    Triangle // 三角形面片，3个控制点
};

// 曲面细分面片（地形用，控制点网格）
struct PatchMesh : GeometryBase {
    uint32_t patchCount = 0; // 面片数量
    PatchType patchType = PatchType::Quad;

    // 获取 D3D12 图元类型（覆盖基类默认值）
    D3D_PRIMITIVE_TOPOLOGY GetPrimitiveTopology() const {
        return (patchType == PatchType::Quad) ? D3D_PRIMITIVE_TOPOLOGY_4_CONTROL_POINT_PATCHLIST
                                              : D3D_PRIMITIVE_TOPOLOGY_3_CONTROL_POINT_PATCHLIST;
    }

    bool IsValid() const { return vertexBufferHandle.IsValid() && indexBufferHandle.IsValid(); }
};

} // namespace DX12Engine::Resource