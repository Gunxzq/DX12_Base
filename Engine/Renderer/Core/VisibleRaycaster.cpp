#include "VisibleRaycaster.h"
#include "CullingSystem.h"
#include "ECS/Core/Components.h"
#include "ECS/Core/Entity.h"
#include "Math/RayIntersection.h"
#include <DirectXMath.h>
#include <algorithm>

using namespace DirectX;
using namespace DX12Engine::ECS;
using namespace DX12Engine::Math;

namespace DX12Engine::Renderer {

void VisibleRaycaster::Initialize(ECS::Registry *registry) { m_registry = registry; }

FRay VisibleRaycaster::ScreenToRay(float screenX, float screenY, uint32_t screenWidth, uint32_t screenHeight) const {
    // NDC: [-1, 1], Y 轴翻转（屏幕左上角为原点）
    float ndcX = (2.0f * screenX) / screenWidth - 1.0f;
    float ndcY = 1.0f - (2.0f * screenY) / screenHeight;

    XMVECTOR nearPointNDC = XMVectorSet(ndcX, ndcY, 0.0f, 1.0f);
    XMVECTOR farPointNDC = XMVectorSet(ndcX, ndcY, 1.0f, 1.0f);

    XMMATRIX invViewProj = m_cameraData.InverseViewProj;

    XMVECTOR nearWorld = XMVector3TransformCoord(nearPointNDC, invViewProj);
    XMVECTOR farWorld = XMVector3TransformCoord(farPointNDC, invViewProj);

    FVector3D origin(XMVectorGetX(nearWorld), XMVectorGetY(nearWorld), XMVectorGetZ(nearWorld));
    FVector3D farPt(XMVectorGetX(farWorld), XMVectorGetY(farWorld), XMVectorGetZ(farWorld));

    FVector3D dir = (farPt - origin).Normalized();

    return {origin, dir};
}

void VisibleRaycaster::CollectHits(const FRay &ray,
                                   std::vector<RaycastHit> &outHits) const {
    if (!m_registry)
        return;

    // 遍历所有可拾取实体（MeshComponent + PickingComponent）
    auto view = m_registry->view<PickingComponent, TransformComponent>();
    for (auto entity : view) {
        auto &pickComp = view.get<PickingComponent>(entity);
        if (!pickComp.isPickable)
            continue;

        auto &transform = view.get<TransformComponent>(entity);

        // 获取包围盒
        const Math::BoundingVolumeVariant *bounds = nullptr;

        if (auto *mesh = m_registry->TryGetComponent<MeshComponent>(entity)) {
            bounds = &mesh->localBounds;
        } else if (auto *transMesh = m_registry->TryGetComponent<TransparentMeshComponent>(entity)) {
            bounds = &transMesh->localBounds;
        } else if (auto *terrain = m_registry->TryGetComponent<TerrainComponent>(entity)) {
            bounds = &terrain->localBounds;
        }

        if (!bounds)
            continue;

        // 将 localBounds 变换到世界空间后测试
        XMMATRIX worldMatrix = transform.GetMatrix();

        float t = 0.0f;
        bool hit = std::visit(
            [&](const auto &b) -> bool {
                using T = std::decay_t<decltype(b)>;

                if constexpr (std::is_same_v<T, Math::BoundingAABB>) {
                    Math::BoundingAABB worldAABB = b;
                    worldAABB.Transform(worldMatrix);
                    return RayIntersectAABB(ray, worldAABB, t);
                } else if constexpr (std::is_same_v<T, Math::BoundingOBB>) {
                    Math::BoundingOBB worldOBB = b;
                    worldOBB.Transform(worldMatrix);
                    return RayIntersectOBB(ray, worldOBB, t);
                } else if constexpr (std::is_same_v<T, Math::BoundingSphere>) {
                    // 变换球心
                    XMVECTOR localCenter = XMVectorSet(b.center.x, b.center.y, b.center.z, 0.0f);
                    XMVECTOR worldCenter = XMVector3TransformCoord(localCenter, worldMatrix);
                    XMVECTOR scale =
                        XMVectorSet(XMVectorGetX(XMVector3Length(XMVectorSet(XMVectorGetX(worldMatrix.r[0]),
                                                                             XMVectorGetY(worldMatrix.r[0]),
                                                                             XMVectorGetZ(worldMatrix.r[0]), 0.0f))),
                                    XMVectorGetX(XMVector3Length(XMVectorSet(XMVectorGetX(worldMatrix.r[1]),
                                                                             XMVectorGetY(worldMatrix.r[1]),
                                                                             XMVectorGetZ(worldMatrix.r[1]), 0.0f))),
                                    XMVectorGetX(XMVector3Length(XMVectorSet(XMVectorGetX(worldMatrix.r[2]),
                                                                             XMVectorGetY(worldMatrix.r[2]),
                                                                             XMVectorGetZ(worldMatrix.r[2]), 0.0f))),
                                    0.0f);
                    float worldRadius =
                        b.radius * std::max({XMVectorGetX(scale), XMVectorGetY(scale), XMVectorGetZ(scale)});

                    FVector3D center(XMVectorGetX(worldCenter), XMVectorGetY(worldCenter), XMVectorGetZ(worldCenter));
                    FVector3D toCenter = center - ray.Origin;
                    float tProj = FVector3D::Dot(toCenter, ray.Direction);
                    FVector3D closest = ray.Origin + ray.Direction * tProj;
                    float distSq = (closest - center).LengthSquared();
                    if (distSq <= worldRadius * worldRadius) {
                        float dt = std::sqrt(worldRadius * worldRadius - distSq);
                        t = (tProj - dt >= 0.0f) ? (tProj - dt) : (tProj + dt);
                        return t >= 0.0f;
                    }
                    return false;
                } else {
                    // 降级：转换到 AABB 再测试
                    Math::BoundingAABB aabb;
                    if constexpr (std::is_same_v<T, Math::BoundingCapsule>) {
                        aabb = b.ToAABB();
                    } else if constexpr (std::is_same_v<T, Math::BoundingConvexHull>) {
                        aabb = b.ToAABB();
                    } else if constexpr (std::is_same_v<T, Math::BoundingCompound>) {
                        aabb = b.ToAABB();
                    }
                    aabb.Transform(worldMatrix);
                    return RayIntersectAABB(ray, aabb, t);
                }
            },
            *bounds);

        if (hit && t >= 0.0f) {
            FVector3D hitPointWorld = ray.Origin + ray.Direction * t;
            outHits.push_back(RaycastHit{
                .entity = entity,
                .distance = t,
                .hitPoint = XMFLOAT3(hitPointWorld.X, hitPointWorld.Y, hitPointWorld.Z),
            });
        }
    }

    // 按距离排序（近→远）
    std::sort(outHits.begin(), outHits.end(),
              [](const RaycastHit &a, const RaycastHit &b) { return a.distance < b.distance; });
}

RaycastResult VisibleRaycaster::RaycastAll(const FRay &ray) const {
    RaycastResult result;
    CollectHits(ray, result.hits);
    return result;
}

} // namespace DX12Engine::Renderer
