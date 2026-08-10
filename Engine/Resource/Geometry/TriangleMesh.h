#pragma once

#include "Common/Common.h"
#include "Math/BoundingVolume.h"
#include "Resource/Core/GpuHandlePool.h"
#include "Resource/Geometry/GeometryBase.h"
#include "Resource/Struct/GeometryHandle.h"
#include <cstdint>
#include <variant>
#include <vector>

namespace DX12Engine {

namespace Math {
using BoundingVolumeVariant = Math::BoundingVolumeVariant;
}
namespace Resource {

// 三角形网格定义（标准三角形网格，.dxmesh 文件加载）
struct TriangleMesh : GeometryBase {
    uint32_t flags = 0; // DxMeshFlags (Skinned, Index16 等)

    bool IsSkinned() const { return (flags & 1) != 0; } // DxMeshFlag_Skinned = 1
};

} // namespace Resource
} // namespace DX12Engine