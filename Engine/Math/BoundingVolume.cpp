#include "BoundingVolume.h"

#include "Common/Common.h"
#include <Jolt/Jolt.h>

#include <Jolt/Geometry/AABox.h>
#include <Jolt/Geometry/ConvexHullBuilder.h>
#include <Jolt/Geometry/Sphere.h>

#include <unordered_set>

namespace DX12Engine::Math {

// ============================================================================
// BoundingSphere
// ============================================================================

void BoundingSphere::ExpandToInclude(const DirectX::XMFLOAT3 &point) {
    JPH::Vec3 jCenter(center.x, center.y, center.z);
    JPH::Vec3 jPoint(point.x, point.y, point.z);
    JPH::Sphere sphere(jCenter, radius);
    sphere.EncapsulatePoint(jPoint);
    center = {sphere.GetCenter().GetX(), sphere.GetCenter().GetY(), sphere.GetCenter().GetZ()};
    radius = sphere.GetRadius();
}

void BoundingSphere::Merge(const BoundingSphere &other) {
    // 手动实现球体合并（Jolt 没有直接提供）
    float dx = other.center.x - center.x;
    float dy = other.center.y - center.y;
    float dz = other.center.z - center.z;
    float dist = sqrtf(dx * dx + dy * dy + dz * dz);

    float newRadius = (dist + radius + other.radius) * 0.5f;

    if (dist > 1e-6f) {
        float t = (newRadius - radius) / dist;
        center.x += dx * t;
        center.y += dy * t;
        center.z += dz * t;
    }

    radius = newRadius;
}

// ============================================================================
// BoundingAABB
// ============================================================================

DirectX::XMFLOAT3 BoundingAABB::GetCenter() const {
    JPH::AABox box(JPH::Vec3(min.x, min.y, min.z), JPH::Vec3(max.x, max.y, max.z));
    JPH::Vec3 center = box.GetCenter();
    return {center.GetX(), center.GetY(), center.GetZ()};
}

DirectX::XMFLOAT3 BoundingAABB::GetExtents() const {
    JPH::AABox box(JPH::Vec3(min.x, min.y, min.z), JPH::Vec3(max.x, max.y, max.z));
    JPH::Vec3 extents = box.GetExtent();
    return {extents.GetX(), extents.GetY(), extents.GetZ()};
}

float BoundingAABB::GetRadius() const {
    JPH::AABox box(JPH::Vec3(min.x, min.y, min.z), JPH::Vec3(max.x, max.y, max.z));
    return box.GetExtent().Length(); // 从中心到角点的距离
}

void BoundingAABB::ExpandToInclude(const DirectX::XMFLOAT3 &point) {
    JPH::AABox box(JPH::Vec3(min.x, min.y, min.z), JPH::Vec3(max.x, max.y, max.z));
    box.Encapsulate(JPH::Vec3(point.x, point.y, point.z));
    min = {box.mMin.GetX(), box.mMin.GetY(), box.mMin.GetZ()};
    max = {box.mMax.GetX(), box.mMax.GetY(), box.mMax.GetZ()};
}

void BoundingAABB::Transform(const DirectX::XMMATRIX &matrix) {
    // 变换 8 个顶点，重新计算 min/max
    DirectX::XMFLOAT3 corners[8] = {{min.x, min.y, min.z}, {max.x, min.y, min.z}, {min.x, max.y, min.z},
                                    {max.x, max.y, min.z}, {min.x, min.y, max.z}, {max.x, min.y, max.z},
                                    {min.x, max.y, max.z}, {max.x, max.y, max.z}};

    JPH::AABox newBox;
    for (int i = 0; i < 8; ++i) {
        DirectX::XMVECTOR v = DirectX::XMLoadFloat3(&corners[i]);
        v = DirectX::XMVector3Transform(v, matrix);
        DirectX::XMFLOAT3 transformed;
        DirectX::XMStoreFloat3(&transformed, v);
        newBox.Encapsulate(JPH::Vec3(transformed.x, transformed.y, transformed.z));
    }

    min = {newBox.mMin.GetX(), newBox.mMin.GetY(), newBox.mMin.GetZ()};
    max = {newBox.mMax.GetX(), newBox.mMax.GetY(), newBox.mMax.GetZ()};
}

BoundingSphere BoundingAABB::ToSphere() const {
    BoundingSphere result;
    result.center = GetCenter();
    result.radius = GetRadius();
    return result;
}

// ============================================================================
// BoundingOBB
// ============================================================================

void BoundingOBB::Transform(const DirectX::XMMATRIX &matrix) {
    // 变换中心
    DirectX::XMVECTOR c = DirectX::XMLoadFloat3(&center);
    c = DirectX::XMVector3Transform(c, matrix);
    DirectX::XMStoreFloat3(&center, c);

    // 变换方向轴
    for (int i = 0; i < 3; ++i) {
        DirectX::XMVECTOR axis = DirectX::XMLoadFloat3(&orientation[i]);
        axis = DirectX::XMVector3TransformNormal(axis, matrix);
        axis = DirectX::XMVector3Normalize(axis);
        DirectX::XMStoreFloat3(&orientation[i], axis);
    }
    // extents 不变（假设无缩放）
}

BoundingSphere BoundingOBB::ToSphere() const {
    JPH::Vec3 jCenter(center.x, center.y, center.z);
    JPH::Vec3 jExtents(extents.x, extents.y, extents.z);
    float radius = jExtents.Length();

    BoundingSphere result;
    result.center = center;
    result.radius = radius;
    return result;
}

BoundingAABB BoundingOBB::ToAABB() const {
    // 计算 OBB 的 8 个顶点
    JPH::Vec3 jCenter(center.x, center.y, center.z);
    JPH::Vec3 jExtents(extents.x, extents.y, extents.z);
    JPH::Vec3 jAxisX(orientation[0].x, orientation[0].y, orientation[0].z);
    JPH::Vec3 jAxisY(orientation[1].x, orientation[1].y, orientation[1].z);
    JPH::Vec3 jAxisZ(orientation[2].x, orientation[2].y, orientation[2].z);

    JPH::AABox aabb;
    for (int ix = -1; ix <= 1; ix += 2) {
        for (int iy = -1; iy <= 1; iy += 2) {
            for (int iz = -1; iz <= 1; iz += 2) {
                JPH::Vec3 vertex = jCenter;
                vertex = vertex + jAxisX * (ix * jExtents.GetX());
                vertex = vertex + jAxisY * (iy * jExtents.GetY());
                vertex = vertex + jAxisZ * (iz * jExtents.GetZ());
                aabb.Encapsulate(vertex);
            }
        }
    }

    BoundingAABB result;
    result.min = {aabb.mMin.GetX(), aabb.mMin.GetY(), aabb.mMin.GetZ()};
    result.max = {aabb.mMax.GetX(), aabb.mMax.GetY(), aabb.mMax.GetZ()};
    return result;
}

// ============================================================================
// BoundingCapsule
// ============================================================================

BoundingSphere BoundingCapsule::ToSphere() const {
    JPH::Vec3 jStart(start.x, start.y, start.z);
    JPH::Vec3 jEnd(end.x, end.y, end.z);
    float halfLen = (jEnd - jStart).Length() * 0.5f;
    JPH::Vec3 jCenter = (jStart + jEnd) * 0.5f;

    BoundingSphere result;
    result.center = {jCenter.GetX(), jCenter.GetY(), jCenter.GetZ()};
    result.radius = halfLen + radius;
    return result;
}

BoundingAABB BoundingCapsule::ToAABB() const {
    JPH::Vec3 jStart(start.x, start.y, start.z);
    JPH::Vec3 jEnd(end.x, end.y, end.z);
    JPH::Vec3 jDir = jEnd - jStart;
    float halfLen = jDir.Length() * 0.5f;
    jDir = jDir.NormalizedOr(JPH::Vec3(0, 1, 0));

    // 胶囊的 AABB = 中心 ± (方向 × 半长 + 半径)
    JPH::Vec3 jCenter = (jStart + jEnd) * 0.5f;
    JPH::Vec3 jExtents = jDir.Abs() * halfLen + JPH::Vec3(radius, radius, radius);

    BoundingAABB result;
    result.min = {jCenter.GetX() - jExtents.GetX(), jCenter.GetY() - jExtents.GetY(), jCenter.GetZ() - jExtents.GetZ()};
    result.max = {jCenter.GetX() + jExtents.GetX(), jCenter.GetY() + jExtents.GetY(), jCenter.GetZ() + jExtents.GetZ()};
    return result;
}

// ============================================================================
// BoundingConvexHull
// ============================================================================

void BoundingConvexHull::ComputeFromVertices(const std::vector<DirectX::XMFLOAT3> &points) {
    if (points.size() < 4) {
        vertices = points;
        return;
    }

    // 转换为 Jolt 格式并保存原始点
    JPH::Array<JPH::Vec3> jPoints;
    jPoints.reserve(points.size());
    for (const auto &p : points) {
        jPoints.push_back(JPH::Vec3(p.x, p.y, p.z));
    }

    // 构建凸包
    JPH::ConvexHullBuilder builder(jPoints);
    const char *error = nullptr;
    auto result = builder.Initialize(INT_MAX, 1e-4f, error);

    if (result == JPH::ConvexHullBuilder::EResult::Success) {
        vertices.clear();

        // 遍历所有面，收集用到的顶点索引
        const auto &faces = builder.GetFaces();
        std::unordered_set<int> usedIndices;

        for (const auto *face : faces) {
            if (face->mRemoved)
                continue;

            JPH::ConvexHullBuilder::Edge *edge = face->mFirstEdge;
            if (edge) {
                do {
                    usedIndices.insert(edge->mStartIdx);
                    edge = edge->mNextEdge;
                } while (edge != face->mFirstEdge);
            }
        }

        // 根据索引从原始点中提取顶点（Jolt 返回的索引指向输入的 jPoints）
        for (int idx : usedIndices) {
            const JPH::Vec3 &v = jPoints[idx];
            vertices.push_back({v.GetX(), v.GetY(), v.GetZ()});
        }
    } else {
        // 构建失败，降级为 AABB 的 8 个顶点
        JPH::AABox aabb;
        for (const auto &p : points) {
            aabb.Encapsulate(JPH::Vec3(p.x, p.y, p.z));
        }
        JPH::Vec3 min = aabb.mMin;
        JPH::Vec3 max = aabb.mMax;

        vertices = {{min.GetX(), min.GetY(), min.GetZ()}, {max.GetX(), min.GetY(), min.GetZ()},
                    {min.GetX(), max.GetY(), min.GetZ()}, {max.GetX(), max.GetY(), min.GetZ()},
                    {min.GetX(), min.GetY(), max.GetZ()}, {max.GetX(), min.GetY(), max.GetZ()},
                    {min.GetX(), max.GetY(), max.GetZ()}, {max.GetX(), max.GetY(), max.GetZ()}};
    }
}

BoundingSphere BoundingConvexHull::ToSphere() const {
    if (vertices.empty())
        return {};

    BoundingSphere result;

    // 1. 寻找距离最远的两个点作为初始直径
    size_t idx1 = 0, idx2 = 0;
    float maxDistSq = -1.0f;
    for (size_t i = 0; i < vertices.size(); ++i) {
        for (size_t j = i + 1; j < vertices.size(); ++j) {
            float dx = vertices[i].x - vertices[j].x;
            float dy = vertices[i].y - vertices[j].y;
            float dz = vertices[i].z - vertices[j].z;
            float distSq = dx * dx + dy * dy + dz * dz;
            if (distSq > maxDistSq) {
                maxDistSq = distSq;
                idx1 = i;
                idx2 = j;
            }
        }
    }

    // 2. 设置初始球体（中点为圆心）
    result.center = {(vertices[idx1].x + vertices[idx2].x) * 0.5f, (vertices[idx1].y + vertices[idx2].y) * 0.5f,
                     (vertices[idx1].z + vertices[idx2].z) * 0.5f};
    result.radius = sqrtf(maxDistSq) * 0.5f;

    // 3. 迭代修正
    for (const auto &v : vertices) {
        float dx = v.x - result.center.x;
        float dy = v.y - result.center.y;
        float dz = v.z - result.center.z;
        float dist = sqrtf(dx * dx + dy * dy + dz * dz);

        if (dist > result.radius) {
            // 扩大半径并移动圆心
            float newRadius = (result.radius + dist) * 0.5f;
            float t = (newRadius - result.radius) / dist;
            result.center.x += dx * t;
            result.center.y += dy * t;
            result.center.z += dz * t;
            result.radius = newRadius;
        }
    }

    return result;
}

BoundingAABB BoundingConvexHull::ToAABB() const {
    if (vertices.empty())
        return {};

    JPH::AABox aabb;
    for (const auto &v : vertices) {
        aabb.Encapsulate(JPH::Vec3(v.x, v.y, v.z));
    }

    BoundingAABB result;
    result.min = {aabb.mMin.GetX(), aabb.mMin.GetY(), aabb.mMin.GetZ()};
    result.max = {aabb.mMax.GetX(), aabb.mMax.GetY(), aabb.mMax.GetZ()};
    return result;
}

// ============================================================================
// BoundingCompound
// ============================================================================

void BoundingCompound::AddSphere(const BoundingSphere &sphere) { spheres.push_back(sphere); }

void BoundingCompound::AddAABB(const BoundingAABB &aabb) { aabbs.push_back(aabb); }

void BoundingCompound::AddOBB(const BoundingOBB &obb) { obbs.push_back(obb); }

void BoundingCompound::AddCapsule(const BoundingCapsule &capsule) { capsules.push_back(capsule); }

BoundingSphere BoundingCompound::ToSphere() const {
    BoundingSphere result;
    bool first = true;

    auto mergeSphere = [&](const BoundingSphere &s) {
        if (first) {
            result = s;
            first = false;
        } else {
            result.Merge(s);
        }
    };

    for (const auto &s : spheres)
        mergeSphere(s);
    for (const auto &a : aabbs)
        mergeSphere(a.ToSphere());
    for (const auto &o : obbs)
        mergeSphere(o.ToSphere());
    for (const auto &c : capsules)
        mergeSphere(c.ToSphere());

    return result;
}

BoundingAABB BoundingCompound::ToAABB() const {
    BoundingAABB result;
    bool first = true;

    auto mergeAABB = [&](const BoundingAABB &a) {
        if (first) {
            result = a;
            first = false;
        } else {
            result.ExpandToInclude(a.min);
            result.ExpandToInclude(a.max);
        }
    };

    for (const auto &s : spheres) {
        BoundingAABB aabb;
        aabb.min = {s.center.x - s.radius, s.center.y - s.radius, s.center.z - s.radius};
        aabb.max = {s.center.x + s.radius, s.center.y + s.radius, s.center.z + s.radius};
        mergeAABB(aabb);
    }
    for (const auto &a : aabbs)
        mergeAABB(a);
    for (const auto &o : obbs)
        mergeAABB(o.ToAABB());
    for (const auto &c : capsules)
        mergeAABB(c.ToAABB());

    return result;
}

} // namespace DX12Engine::Math