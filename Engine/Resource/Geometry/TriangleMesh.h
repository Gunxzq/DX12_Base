// TriangleMesh.h
#pragma once
#include "Common/Common.h"
#include "Renderer/Scene/Struct/BoundingVolume.h"
#include "Resource/Struct/GeometryHandle.h"
#include "Resource/Struct/ResourceHandle.h"
#include <cstdint>
#include <variant>

namespace DX12Engine::Resource {

using BoundingVolumeVariant =
    std::variant<DX12Engine::Renderer::BoundingSphere, DX12Engine::Renderer::BoundingAABB,
                 DX12Engine::Renderer::BoundingOBB, DX12Engine::Renderer::BoundingCapsule,
                 DX12Engine::Renderer::BoundingConvexHull, DX12Engine::Renderer::BoundingCompound>;

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

    BoundingVolumeVariant localBounds;

    bool isGpuReady = false;
};

} // namespace DX12Engine::Resource