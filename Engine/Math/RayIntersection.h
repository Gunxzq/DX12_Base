#pragma once

#include "Math/MathTypes.h"
#include "Math/BoundingVolume.h"
#include <DirectXMath.h>
#include <algorithm>

namespace DX12Engine::Math {

// ============================================================================
// 射线与包围盒相交测试
// ============================================================================

/**
 * @brief 射线 vs AABB（Slab 方法）
 * @return true 如果相交，outT 为最近交点参数 t
 */
inline bool RayIntersectAABB(const FRay &ray, const BoundingAABB &aabb, float &outT) {
    float tMin = -FLT_MAX;
    float tMax = FLT_MAX;

    // X slab
    if (std::abs(ray.Direction.X) > 1e-6f) {
        float t1 = (aabb.min.x - ray.Origin.X) / ray.Direction.X;
        float t2 = (aabb.max.x - ray.Origin.X) / ray.Direction.X;
        if (t1 > t2) std::swap(t1, t2);
        tMin = std::max(tMin, t1);
        tMax = std::min(tMax, t2);
    } else {
        if (ray.Origin.X < aabb.min.x || ray.Origin.X > aabb.max.x) return false;
    }

    // Y slab
    if (std::abs(ray.Direction.Y) > 1e-6f) {
        float t1 = (aabb.min.y - ray.Origin.Y) / ray.Direction.Y;
        float t2 = (aabb.max.y - ray.Origin.Y) / ray.Direction.Y;
        if (t1 > t2) std::swap(t1, t2);
        tMin = std::max(tMin, t1);
        tMax = std::min(tMax, t2);
    } else {
        if (ray.Origin.Y < aabb.min.y || ray.Origin.Y > aabb.max.y) return false;
    }

    // Z slab
    if (std::abs(ray.Direction.Z) > 1e-6f) {
        float t1 = (aabb.min.z - ray.Origin.Z) / ray.Direction.Z;
        float t2 = (aabb.max.z - ray.Origin.Z) / ray.Direction.Z;
        if (t1 > t2) std::swap(t1, t2);
        tMin = std::max(tMin, t1);
        tMax = std::min(tMax, t2);
    } else {
        if (ray.Origin.Z < aabb.min.z || ray.Origin.Z > aabb.max.z) return false;
    }

    if (tMin <= tMax && tMax >= 0.0f) {
        outT = (tMin >= 0.0f) ? tMin : tMax;
        return true;
    }
    return false;
}

/**
 * @brief 射线 vs OBB（变换到局部空间后 Slab 测试）
 */
inline bool RayIntersectOBB(const FRay &ray, const BoundingOBB &obb, float &outT) {
    using namespace DirectX;

    XMVECTOR rayOrigin = XMVectorSet(ray.Origin.X, ray.Origin.Y, ray.Origin.Z, 0.0f);
    XMVECTOR rayDir = XMVectorSet(ray.Direction.X, ray.Direction.Y, ray.Direction.Z, 0.0f);
    XMVECTOR center = XMVectorSet(obb.center.x, obb.center.y, obb.center.z, 0.0f);
    XMVECTOR ext = XMVectorSet(obb.extents.x, obb.extents.y, obb.extents.z, 0.0f);

    XMVECTOR X = XMVectorSet(obb.orientation[0].x, obb.orientation[0].y, obb.orientation[0].z, 0.0f);
    XMVECTOR Y = XMVectorSet(obb.orientation[1].x, obb.orientation[1].y, obb.orientation[1].z, 0.0f);
    XMVECTOR Z = XMVectorSet(obb.orientation[2].x, obb.orientation[2].y, obb.orientation[2].z, 0.0f);

    float tMin = -FLT_MAX;
    float tMax = FLT_MAX;

    XMVECTOR diff = XMVectorSubtract(rayOrigin, center);

    // Test on each of the three OBB axes
    struct { XMVECTOR axis; float extent; } axes[3] = {
        {X, XMVectorGetX(ext)},
        {Y, XMVectorGetY(ext)},
        {Z, XMVectorGetZ(ext)}
    };

    for (int i = 0; i < 3; ++i) {
        float e = XMVectorGetX(XMVector3Dot(diff, axes[i].axis));
        float f = XMVectorGetX(XMVector3Dot(rayDir, axes[i].axis));

        if (std::abs(f) > 1e-6f) {
            float t1 = (e + axes[i].extent) / f;
            float t2 = (e - axes[i].extent) / f;
            if (t1 > t2) std::swap(t1, t2);
            tMin = std::max(tMin, t1);
            tMax = std::min(tMax, t2);
        } else {
            if (-e - axes[i].extent > 0.0f || -e + axes[i].extent < 0.0f)
                return false;
        }

        if (tMin > tMax) return false;
    }

    if (tMax >= 0.0f) {
        outT = (tMin >= 0.0f) ? tMin : tMax;
        return true;
    }
    return false;
}

/**
 * @brief 射线 vs BoundingVolumeVariant 分发
 */
inline bool RayIntersectBounds(const FRay &ray, const BoundingVolumeVariant &bounds, float &outT) {
    return std::visit([&](const auto &b) -> bool {
        using T = std::decay_t<decltype(b)>;
        if constexpr (std::is_same_v<T, BoundingAABB>) {
            return RayIntersectAABB(ray, b, outT);
        } else if constexpr (std::is_same_v<T, BoundingOBB>) {
            return RayIntersectOBB(ray, b, outT);
        } else if constexpr (std::is_same_v<T, BoundingSphere>) {
            // Sphere test
            FVector3D toCenter(b.center.x - ray.Origin.X, b.center.y - ray.Origin.Y, b.center.z - ray.Origin.Z);
            float tProj = FVector3D::Dot(toCenter, ray.Direction);
            FVector3D closestPoint = ray.Origin + ray.Direction * tProj;
            float distSq = (closestPoint - FVector3D(b.center.x, b.center.y, b.center.z)).LengthSquared();
            if (distSq <= b.radius * b.radius) {
                float dt = std::sqrt(b.radius * b.radius - distSq);
                outT = (tProj - dt >= 0.0f) ? (tProj - dt) : (tProj + dt);
                return outT >= 0.0f;
            }
            return false;
        } else {
            // 降级为 AABB
            BoundingAABB aabb;
            if constexpr (std::is_same_v<T, BoundingCapsule>) {
                aabb = b.ToAABB();
            } else if constexpr (std::is_same_v<T, BoundingConvexHull>) {
                aabb = b.ToAABB();
            } else if constexpr (std::is_same_v<T, BoundingCompound>) {
                aabb = b.ToAABB();
            }
            return RayIntersectAABB(ray, aabb, outT);
        }
    }, bounds);
}

} // namespace DX12Engine::Math
