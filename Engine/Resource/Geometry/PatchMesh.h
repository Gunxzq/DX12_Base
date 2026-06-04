// PatchMesh.h
#pragma once

#include "Math/BoundingVolume.h"
#include "Resource/Struct/ResourceHandle.h"
#include <DirectXMath.h>
#include <d3d12.h>

namespace DX12Engine::Resource {

enum class PatchType : uint8_t {
    Quad,    // 四边形面片，4个控制点
    Triangle // 三角形面片，3个控制点
};

struct PatchMesh {
    // GPU 资源句柄
    GpuResourceHandle vertexBufferHandle;
    GpuResourceHandle indexBufferHandle;

    // 几何数据
    uint32_t vertexCount = 0;
    uint32_t indexCount = 0; // 面片索引数量
    uint32_t patchCount = 0; // 面片数量
    uint32_t vertexStride = 0;
    DXGI_FORMAT indexFormat = DXGI_FORMAT_R32_UINT;

    // 拓扑类型
    PatchType patchType = PatchType::Quad;

    // 获取 D3D12 图元类型
    D3D12_PRIMITIVE_TOPOLOGY GetPrimitiveTopology() const {
        return (patchType == PatchType::Quad) ? D3D_PRIMITIVE_TOPOLOGY_4_CONTROL_POINT_PATCHLIST
                                              : D3D_PRIMITIVE_TOPOLOGY_3_CONTROL_POINT_PATCHLIST;
    }

    // 包围盒
    Math::BoundingVolumeVariant localBounds;

    // 状态
    bool isGpuReady = false;

    bool IsValid() const { return vertexBufferHandle.IsValid() && indexBufferHandle.IsValid(); }
};

} // namespace DX12Engine::Resource