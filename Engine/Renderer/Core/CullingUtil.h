#pragma once

#include "ECS/Core/Components.h"
#include "Math/BoundingVolume.h"
#include "Renderer/Core/LODConfig.h"
#include "Renderer/Scene/Struct/Frustum.h"
#include "Resource/Asset/LODMesh.h"
#include "Resource/Struct/LODMeshHandle.h"
#include <DirectXMath.h>

namespace DX12Engine::Renderer {

// ============================================================================
// 视锥剔除工具函数
// ============================================================================

/// 测试 AABB vs 视锥体（6 平面法），扩展 15% 保护带
inline bool FrustumCullAABB(const Math::BoundingAABB &bounds, const Frustum &frustum) {
    const auto &planes = frustum.GetPlanes();
    DirectX::XMFLOAT3 centerF3 = bounds.GetCenter();
    DirectX::XMFLOAT3 extentsF3 = bounds.GetExtents();
    DirectX::XMVECTOR center = DirectX::XMLoadFloat3(&centerF3);
    DirectX::XMVECTOR extents = DirectX::XMLoadFloat3(&extentsF3);
    DirectX::XMVECTOR expand = DirectX::XMVectorReplicate(1.15f);
    extents = DirectX::XMVectorMultiply(extents, expand);

    DirectX::XMVECTOR aabbMin = DirectX::XMVectorSubtract(center, extents);
    DirectX::XMVECTOR aabbMax = DirectX::XMVectorAdd(center, extents);

    for (int i = 0; i < 6; ++i) {
        DirectX::XMVECTOR plane = planes[i];
        DirectX::XMVECTOR planeNormal = DirectX::XMVectorSet(DirectX::XMVectorGetX(plane), DirectX::XMVectorGetY(plane),
                                                             DirectX::XMVectorGetZ(plane), 0.0f);
        DirectX::XMVECTOR pVertex =
            DirectX::XMVectorSelect(aabbMin, aabbMax, DirectX::XMVectorGreater(planeNormal, DirectX::XMVectorZero()));
        DirectX::XMVECTOR pVertex4 = DirectX::XMVectorSetW(pVertex, 1.0f);
        float distance = DirectX::XMVectorGetX(DirectX::XMPlaneDotCoord(plane, pVertex));
        if (distance < 0.0f)
            return false;
    }
    return true;
}

/// 测试球体 vs 视锥体，扩展 15% 保护带
inline bool FrustumCullSphere(const Math::BoundingSphere &bounds, const Frustum &frustum) {
    const auto &planes = frustum.GetPlanes();
    float expandedRadius = bounds.radius * 1.15f;
    DirectX::XMVECTOR sphereCenter = DirectX::XMLoadFloat3(&bounds.center);
    for (int i = 0; i < 6; ++i) {
        DirectX::XMVECTOR plane = planes[i];
        DirectX::XMVECTOR center4 = DirectX::XMVectorSetW(sphereCenter, 1.0f);
        float distance = DirectX::XMVectorGetX(DirectX::XMPlaneDotCoord(plane, center4));
        if (distance < -expandedRadius)
            return false;
    }
    return true;
}

/// 通用 vs 视锥体测试（BoundingVolumeVariant → 世界空间）
inline bool FrustumCull(const Math::BoundingVolumeVariant &localBounds, const DirectX::XMMATRIX &worldMatrix,
                        const Frustum &frustum) {
    return std::visit(
        [&](const auto &b) -> bool {
            using T = std::decay_t<decltype(b)>;
            if constexpr (std::is_same_v<T, Math::BoundingAABB>) {
                Math::BoundingAABB worldBounds = b;
                worldBounds.Transform(worldMatrix);
                return FrustumCullAABB(worldBounds, frustum);
            } else if constexpr (std::is_same_v<T, Math::BoundingSphere>) {
                DirectX::XMVECTOR center = DirectX::XMLoadFloat3(&b.center);
                center = DirectX::XMVector3Transform(center, worldMatrix);
                DirectX::XMFLOAT3 worldCenter;
                DirectX::XMStoreFloat3(&worldCenter, center);
                DirectX::XMVECTOR scaleVec = DirectX::XMVectorSet(DirectX::XMVectorGetX(worldMatrix.r[0]),
                                                                  DirectX::XMVectorGetY(worldMatrix.r[1]),
                                                                  DirectX::XMVectorGetZ(worldMatrix.r[2]), 0.0f);
                float maxScale =
                    std::max({std::abs(DirectX::XMVectorGetX(scaleVec)), std::abs(DirectX::XMVectorGetY(scaleVec)),
                              std::abs(DirectX::XMVectorGetZ(scaleVec))});
                Math::BoundingSphere worldSphere;
                worldSphere.center = worldCenter;
                worldSphere.radius = b.radius * maxScale;
                return FrustumCullSphere(worldSphere, frustum);
            } else if constexpr (std::is_same_v<T, Math::BoundingOBB>) {
                Math::BoundingOBB worldOBB = b;
                worldOBB.Transform(worldMatrix);
                Math::BoundingAABB worldAABB = worldOBB.ToAABB();
                return FrustumCullAABB(worldAABB, frustum);
            } else if constexpr (std::is_same_v<T, Math::BoundingCapsule>) {
                Math::BoundingSphere sphere = b.ToSphere();
                DirectX::XMVECTOR center = DirectX::XMLoadFloat3(&sphere.center);
                center = DirectX::XMVector3Transform(center, worldMatrix);
                DirectX::XMFLOAT3 worldCenter;
                DirectX::XMStoreFloat3(&worldCenter, center);
                DirectX::XMVECTOR scaleVec = DirectX::XMVectorSet(DirectX::XMVectorGetX(worldMatrix.r[0]),
                                                                  DirectX::XMVectorGetY(worldMatrix.r[1]),
                                                                  DirectX::XMVectorGetZ(worldMatrix.r[2]), 0.0f);
                float maxScale =
                    std::max({std::abs(DirectX::XMVectorGetX(scaleVec)), std::abs(DirectX::XMVectorGetY(scaleVec)),
                              std::abs(DirectX::XMVectorGetZ(scaleVec))});
                Math::BoundingSphere worldSphere;
                worldSphere.center = worldCenter;
                worldSphere.radius = sphere.radius * maxScale;
                return FrustumCullSphere(worldSphere, frustum);
            } else if constexpr (std::is_same_v<T, Math::BoundingConvexHull>) {
                Math::BoundingSphere sphere = b.ToSphere();
                DirectX::XMVECTOR center = DirectX::XMLoadFloat3(&sphere.center);
                center = DirectX::XMVector3Transform(center, worldMatrix);
                DirectX::XMFLOAT3 worldCenter;
                DirectX::XMStoreFloat3(&worldCenter, center);
                DirectX::XMVECTOR scaleVec = DirectX::XMVectorSet(DirectX::XMVectorGetX(worldMatrix.r[0]),
                                                                  DirectX::XMVectorGetY(worldMatrix.r[1]),
                                                                  DirectX::XMVectorGetZ(worldMatrix.r[2]), 0.0f);
                float maxScale =
                    std::max({std::abs(DirectX::XMVectorGetX(scaleVec)), std::abs(DirectX::XMVectorGetY(scaleVec)),
                              std::abs(DirectX::XMVectorGetZ(scaleVec))});
                Math::BoundingSphere worldSphere;
                worldSphere.center = worldCenter;
                worldSphere.radius = sphere.radius * maxScale;
                return FrustumCullSphere(worldSphere, frustum);
            } else if constexpr (std::is_same_v<T, Math::BoundingCompound>) {
                Math::BoundingAABB worldAABB = b.ToAABB();
                worldAABB.Transform(worldMatrix);
                return FrustumCullAABB(worldAABB, frustum);
            } else {
                return true; // 未知类型，保守通过
            }
        },
        localBounds);
}

// ============================================================================
// LOD 选择工具函数
// ============================================================================

/// 根据距离选择 LOD，返回 GeometryHandle
inline Resource::GeometryHandle PickLOD(const Resource::LODMesh &lodMesh, const DirectX::XMFLOAT3 &entityPos,
                                        const DirectX::XMFLOAT3 &cameraPos, const LODConfig &lodConfig) {
    float dx = entityPos.x - cameraPos.x;
    float dy = entityPos.y - cameraPos.y;
    float dz = entityPos.z - cameraPos.z;
    float distance = std::sqrt(dx * dx + dy * dy + dz * dz);

    uint32_t requestedIndex = lodConfig.GetLODIndex(distance);
    uint32_t meshLODCount = static_cast<uint32_t>(lodMesh.GetLODCount());

    if (requestedIndex < meshLODCount) {
        Resource::GeometryHandle handle = lodMesh.GetLODByIndex(requestedIndex);
        if (handle.IsValid())
            return handle;
    }
    if (meshLODCount > 0)
        return lodMesh.GetLowestLOD();

    return Resource::GeometryHandle::Invalid();
}

} // namespace DX12Engine::Renderer
