#include "CullingSystem.h"

#include "Common/Common.h"

#include "ECS/Core/Components.h"
#include "Renderer/Scene/CameraManager.h"
#include "Scheduler/FrameDriver.h"

#include <algorithm>
#include <cstdio>
#include <variant>

using namespace DX12Engine::Math;

namespace DX12Engine::Renderer {

// ============================================================================
// 构建预测视锥体（相机运动补偿）
// ============================================================================

Frustum CullingSystem::BuildPredictedFrustum(float dt) const {
    if (!m_cameraManager) {
        // 无相机管理器时回退：直接从主相机构建（无预测）
        return Frustum{};
    }

    const Camera &camera = m_cameraManager->GetMainCamera();

    // 计算预测位置：PredictedPos = P + V * dt * K
    float dtK = dt * m_predictionFactor;
    DirectX::XMFLOAT3 predictedPos;
    predictedPos.x = camera.Position.x + camera.Velocity.x * dtK;
    predictedPos.y = camera.Position.y + camera.Velocity.y * dtK;
    predictedPos.z = camera.Position.z + camera.Velocity.z * dtK;

    // 可选：扩大 FOV 增加安全余量（变速/转向场景）
    float predictedFOV = camera.FOV * 1.05f; // 略微扩大 5%

    Frustum frustum;
    frustum.BuildFromCamera(predictedPos, camera.Forward, camera.Up, predictedFOV, camera.AspectRatio, camera.NearPlane,
                            camera.FarPlane);
    return frustum;
}

// ============================================================================
// 执行
// ============================================================================

void CullingSystem::Execute(ECS::Registry &registry, CullingResult &outResult) {
    outResult.Clear();

    // 如果没有设置视锥体，所有实体可见（回退行为）
    if (!m_frustum) {
        auto allView = registry.view<ECS::MeshComponent, ECS::TransformComponent>();
        for (auto entity : allView) {
            outResult.SetVisible(entity);
        }
        auto transparentView = registry.view<ECS::TransparentMeshComponent, ECS::TransformComponent>();
        for (auto entity : transparentView) {
            outResult.SetVisible(entity);
        }
        auto terrainView = registry.view<ECS::TerrainComponent, ECS::TransformComponent>();
        for (auto entity : terrainView) {
            outResult.SetVisible(entity);
        }
        auto billboardView = registry.view<ECS::BillboardComponent, ECS::TransformComponent>();
        for (auto entity : billboardView) {
            outResult.SetVisible(entity);
        }
        static int s_noFrustumLogCount = 0;
        if (s_noFrustumLogCount++ < 3) {
            OutputDebugStringA("[CullingSystem] No frustum set, all entities visible (fallback)\n");
        }
        return;
    }

    const Frustum &frustum = *m_frustum;

    // 不透明网格
    {
        auto view = registry.view<ECS::MeshComponent, ECS::TransformComponent>();
        for (auto entity : view) {
            auto &meshComp = view.get<ECS::MeshComponent>(entity);
            auto &transform = view.get<ECS::TransformComponent>(entity);
            bool visible = TestVisibility(frustum, meshComp.localBounds, transform.GetMatrix());

            if (visible)
                outResult.SetVisible(entity);
        }
    }

    // 透明网格
    {
        auto view = registry.view<ECS::TransparentMeshComponent, ECS::TransformComponent>();
        for (auto entity : view) {
            auto &comp = view.get<ECS::TransparentMeshComponent>(entity);
            auto &transform = view.get<ECS::TransformComponent>(entity);
            bool visible = TestVisibility(frustum, comp.localBounds, transform.GetMatrix());

            if (visible)
                outResult.SetVisible(entity);
        }
    }

    // 地形
    {
        auto view = registry.view<ECS::TerrainComponent, ECS::TransformComponent>();
        for (auto entity : view) {
            auto &comp = view.get<ECS::TerrainComponent>(entity);
            auto &transform = view.get<ECS::TransformComponent>(entity);
            bool visible = TestVisibility(frustum, comp.localBounds, transform.GetMatrix());

            if (visible)
                outResult.SetVisible(entity);
        }
    }

    // 公告牌（使用扩展球体：以 width/height 为半径的宽松包围球）
    {
        auto view = registry.view<ECS::BillboardComponent, ECS::TransformComponent>();
        for (auto entity : view) {
            auto &comp = view.get<ECS::BillboardComponent>(entity);
            auto &transform = view.get<ECS::TransformComponent>(entity);

            // 公告牌没有 localBounds，构造一个宽松的包围球
            float maxDim = std::max(comp.width, comp.height) * 0.5f;
            Math::BoundingSphere billboardSphere;
            billboardSphere.center = transform.position;
            billboardSphere.radius = maxDim * std::max({transform.scale.x, transform.scale.y, transform.scale.z});

            if (Intersects(frustum, billboardSphere))
                outResult.SetVisible(entity);
        }
    }
}

// ============================================================================
// 辅助方法
// ============================================================================

bool CullingSystem::Intersects(const Frustum &frustum, const Math::BoundingAABB &bounds) const {
    // 标准 AABB vs 视锥体测试（6 平面法）
    // 对于每个平面，计算 AABB 在平面法线方向上的最远点（p-vertex）
    // 如果最远点仍在平面外侧（dot < 0），则完全不可见
    const auto &planes = frustum.GetPlanes();

    // AABB 的 8 个角点在平面法线方向上的符号：正分量取 max，负分量取 min
    DirectX::XMVECTOR aabbMin = DirectX::XMLoadFloat3(&bounds.min);
    DirectX::XMVECTOR aabbMax = DirectX::XMLoadFloat3(&bounds.max);

    for (int i = 0; i < 6; ++i) {
        DirectX::XMVECTOR plane = planes[i];
        DirectX::XMVECTOR planeNormal = DirectX::XMVectorSet(DirectX::XMVectorGetX(plane), DirectX::XMVectorGetY(plane),
                                                             DirectX::XMVectorGetZ(plane), 0.0f);

        // p-vertex: 平面法线分量 > 0 取 max，<= 0 取 min
        DirectX::XMVECTOR pVertex =
            DirectX::XMVectorSelect(aabbMin, aabbMax, DirectX::XMVectorGreater(planeNormal, DirectX::XMVectorZero()));

        // 点到平面的有符号距离：dot(plane, (px, py, pz, 1))
        DirectX::XMVECTOR pVertex4 = DirectX::XMVectorSetW(pVertex, 1.0f);
        float distance = DirectX::XMVectorGetX(DirectX::XMPlaneDotCoord(plane, pVertex));

        // 如果最远点都在平面外侧，则完全不可见
        if (distance < 0.0f) {
            return false;
        }
    }

    return true;
}

bool CullingSystem::Intersects(const Frustum &frustum, const Math::BoundingSphere &bounds) const {
    // 球体 vs 视锥体测试
    // 计算球心到每个平面的有符号距离
    // 如果距离 < -半径，则不可见
    const auto &planes = frustum.GetPlanes();

    DirectX::XMVECTOR sphereCenter = DirectX::XMLoadFloat3(&bounds.center);

    for (int i = 0; i < 6; ++i) {
        DirectX::XMVECTOR plane = planes[i];
        DirectX::XMVECTOR center4 = DirectX::XMVectorSetW(sphereCenter, 1.0f);
        float distance = DirectX::XMVectorGetX(DirectX::XMPlaneDotCoord(plane, center4));

        if (distance < -bounds.radius) {
            return false; // 球体完全在平面外侧
        }
    }

    return true;
}

bool CullingSystem::TestVisibility(const Frustum &frustum, const Math::BoundingVolumeVariant &bounds,
                                   const DirectX::XMMATRIX &worldMatrix) const {
    // 使用 std::visit 根据包围盒类型分发到对应的相交测试
    return std::visit(
        [&](const auto &b) -> bool {
            using T = std::decay_t<decltype(b)>;

            if constexpr (std::is_same_v<T, Math::BoundingAABB>) {
                // 将局部 AABB 变换到世界空间后测试
                Math::BoundingAABB worldBounds = b;
                worldBounds.Transform(worldMatrix);
                return Intersects(frustum, worldBounds);
            } else if constexpr (std::is_same_v<T, Math::BoundingSphere>) {
                // 将球心变换到世界空间（半径受缩放影响）
                DirectX::XMVECTOR center = DirectX::XMLoadFloat3(&b.center);
                center = DirectX::XMVector3Transform(center, worldMatrix);
                DirectX::XMFLOAT3 worldCenter;
                DirectX::XMStoreFloat3(&worldCenter, center);

                // 取缩放因子的最大值作为半径缩放
                DirectX::XMVECTOR scaleVec = DirectX::XMVectorSet(DirectX::XMVectorGetX(worldMatrix.r[0]),
                                                                  DirectX::XMVectorGetY(worldMatrix.r[1]),
                                                                  DirectX::XMVectorGetZ(worldMatrix.r[2]), 0.0f);
                float maxScale =
                    std::max({std::abs(DirectX::XMVectorGetX(scaleVec)), std::abs(DirectX::XMVectorGetY(scaleVec)),
                              std::abs(DirectX::XMVectorGetZ(scaleVec))});

                Math::BoundingSphere worldSphere;
                worldSphere.center = worldCenter;
                worldSphere.radius = b.radius * maxScale;
                return Intersects(frustum, worldSphere);
            } else if constexpr (std::is_same_v<T, Math::BoundingOBB>) {
                // OBB → AABB 再测试（保守近似）
                Math::BoundingOBB worldOBB = b;
                worldOBB.Transform(worldMatrix);
                Math::BoundingAABB worldAABB = worldOBB.ToAABB();
                return Intersects(frustum, worldAABB);
            } else if constexpr (std::is_same_v<T, Math::BoundingCapsule>) {
                // 胶囊体 → 球体（保守近似）
                Math::BoundingSphere sphere = b.ToSphere();
                // 变换到世界空间
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
                return Intersects(frustum, worldSphere);
            } else if constexpr (std::is_same_v<T, Math::BoundingConvexHull>) {
                // 凸包 → 球体（保守近似）
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
                return Intersects(frustum, worldSphere);
            } else if constexpr (std::is_same_v<T, Math::BoundingCompound>) {
                // 复合包围盒：降级为 AABB 测试
                Math::BoundingAABB worldAABB = b.ToAABB();
                worldAABB.Transform(worldMatrix);
                return Intersects(frustum, worldAABB);
            } else {
                // 未知类型，保守通过
                return true;
            }
        },
        bounds);
}

} // namespace DX12Engine::Renderer