// Renderer/Core/CullingSystem.cpp
#include "CullingSystem.h"
#include "ECS/Core/Components.h"

using namespace DX12Engine::Math;

namespace DX12Engine::Renderer {

// ============================================================================
// 执行
// ============================================================================

void CullingSystem::Execute(ECS::Registry &registry, CullingResult &outResult) {
    // TODO: 实现视锥体剔除逻辑
    // 当前阶段：所有实体都视为可见
    //
    // 完整实现步骤：
    // 1. 检查视锥体是否有效
    //    if (!m_frustum) return;
    //
    // 2. 获取相机位置（用于世界空间包围盒变换）
    //    const auto& camera = m_cameraManager->GetMainCamera();
    //
    // 3. 遍历所有带 MeshComponent 和 TransformComponent 的实体
    //    auto view = registry.view<ECS::MeshComponent, ECS::TransformComponent>();
    //
    // 4. 对每个实体：
    //    - 获取局部包围盒 meshComp.localBounds
    //    - 根据 TransformComponent 变换到世界空间
    //    - 调用 TestVisibility 进行测试
    //    - 将结果存入 outResult
    //
    // 5. 注意：CullingSystem 不修改 ECS 组件，只输出到临时结果容器
    //    结果由 RenderItemBuilder 消费

    outResult.Clear();

    auto view = registry.view<ECS::MeshComponent, ECS::TransformComponent>();
    for (auto entity : view) {
        // 当前阶段：所有实体都可见
        outResult.SetVisible(entity);
    }
}

// ============================================================================
// 辅助方法
// ============================================================================

bool CullingSystem::Intersects(const Frustum &frustum, const Math::BoundingAABB &bounds) const {
    // TODO: 实现 AABB 与视锥体的相交测试
    // 当前阶段：始终返回 true
    //
    // 算法简述：
    // 对视锥体的 6 个平面，计算 AABB 在平面法线方向上的最远点
    // 如果该最远点在平面外侧（距离 < 0），则完全不可见
    // 如果所有平面都通过测试，则至少部分可见
    return true;
}

bool CullingSystem::Intersects(const Frustum &frustum, const Math::BoundingSphere &bounds) const {
    // TODO: 实现球体与视锥体的相交测试
    // 当前阶段：始终返回 true
    //
    // 算法简述：
    // 计算球心到每个平面的有符号距离
    // 如果距离 < -半径，则在平面外侧，不可见
    // 如果距离 < 半径，则相交
    return true;
}

bool CullingSystem::TestVisibility(const Frustum &frustum, const Math::BoundingVolumeVariant &bounds,
                                   const DirectX::XMFLOAT3 &worldPosition) const {
    // TODO: 根据 bounds 类型分发到对应的相交测试
    // 当前阶段：始终返回 true
    //
    // 完整实现需要使用 std::visit 访问 variant 中的具体类型
    // 例如：
    // return std::visit([&](const auto& b) -> bool {
    //     if constexpr (std::is_same_v<std::decay_t<decltype(b)>, BoundingAABB>) {
    //         // 将局部 AABB 变换到世界空间后再测试
    //         BoundingAABB worldBounds = TransformAABB(b, worldPosition);
    //         return Intersects(frustum, worldBounds);
    //     } else if constexpr (std::is_same_v<...>) {
    //         // 其他类型...
    //     }
    // }, bounds);
    return true;
}

} // namespace DX12Engine::Renderer