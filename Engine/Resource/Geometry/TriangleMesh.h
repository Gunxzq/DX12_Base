#pragma once

#include "Common/Common.h"
#include "Math/BoundingVolume.h"
#include "Resource/Struct/GeometryHandle.h"
#include "Resource/Core/GpuHandlePool.h"
#include <cstdint>
#include <variant>

namespace DX12Engine {

namespace Math {
using BoundingVolumeVariant = Math::BoundingVolumeVariant;
}
namespace Resource {
// 三角形网格定义
struct TriangleMesh {
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

    bool isGpuReady = false;
};

} // namespace Resource
} // namespace DX12Engine