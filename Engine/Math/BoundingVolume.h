#pragma once

#include <DirectXMath.h>
#include <variant>
#include <vector>

namespace DX12Engine::Math {

// ============================================================================
// 包围盒类型枚举
// ============================================================================

enum class BoundsType : uint8_t { None = 0, Sphere, AABB, OBB, Capsule, ConvexHull, Compound };

// ============================================================================
// 1. 球形包围盒
// ============================================================================

struct BoundingSphere {
    DirectX::XMFLOAT3 center = {0.0f, 0.0f, 0.0f}; // 中心坐标
    float radius = 0.0f;                           // 半径

    void ExpandToInclude(const DirectX::XMFLOAT3 &point);
    void Merge(const BoundingSphere &other);
};

// ============================================================================
// 2. 轴对齐包围盒
// ============================================================================

struct BoundingAABB {
    DirectX::XMFLOAT3 min = {0.0f, 0.0f, 0.0f};
    DirectX::XMFLOAT3 max = {0.0f, 0.0f, 0.0f};

    DirectX::XMFLOAT3 GetCenter() const;
    DirectX::XMFLOAT3 GetExtents() const;

    float GetRadius() const;
    void ExpandToInclude(const DirectX::XMFLOAT3 &point);
    void Transform(const DirectX::XMMATRIX &matrix);
    BoundingSphere ToSphere() const;
};

// ============================================================================
// 3. 有向包围盒
// ============================================================================

struct BoundingOBB {
    DirectX::XMFLOAT3 center = {0.0f, 0.0f, 0.0f};
    DirectX::XMFLOAT3 extents = {0.5f, 0.5f, 0.5f};
    DirectX::XMFLOAT3 orientation[3] = {{1.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f}, {0.0f, 0.0f, 1.0f}};

    void Transform(const DirectX::XMMATRIX &matrix);

    BoundingAABB ToAABB() const;
    BoundingSphere ToSphere() const;
};

// ============================================================================
// 4. 胶囊体包围盒
// ============================================================================

struct BoundingCapsule {
    DirectX::XMFLOAT3 start = {0.0f, 0.0f, 0.0f};
    DirectX::XMFLOAT3 end = {0.0f, 1.0f, 0.0f};
    float radius = 0.5f;

    BoundingSphere ToSphere() const;
    BoundingAABB ToAABB() const;
};

// ============================================================================
// 5. 凸包包围盒
// ============================================================================

struct BoundingConvexHull {
    std::vector<DirectX::XMFLOAT3> vertices;

    void ComputeFromVertices(const std::vector<DirectX::XMFLOAT3> &points);

    BoundingSphere ToSphere() const;
    BoundingAABB ToAABB() const;
};

// ============================================================================
// 6. 复合包围盒
// ============================================================================

struct BoundingCompound {
    std::vector<BoundingSphere> spheres;
    std::vector<BoundingAABB> aabbs;
    std::vector<BoundingOBB> obbs;
    std::vector<BoundingCapsule> capsules;

    void AddSphere(const BoundingSphere &sphere);
    void AddAABB(const BoundingAABB &aabb);
    void AddOBB(const BoundingOBB &obb);
    void AddCapsule(const BoundingCapsule &capsule);

    BoundingSphere ToSphere() const;
    BoundingAABB ToAABB() const;
};

// ============================================================================
// 包围盒类型擦除（用于组件存储）
// ============================================================================

using BoundingVolumeVariant =
    std::variant<BoundingSphere, BoundingAABB, BoundingOBB, BoundingCapsule, BoundingConvexHull, BoundingCompound>;

} // namespace DX12Engine::Math