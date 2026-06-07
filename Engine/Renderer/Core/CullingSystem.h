#pragma once

#include "ECS/Core/Components.h"
#include "ECS/Core/Registry.h"
#include "Math/BoundingVolume.h"
#include "Renderer/Scene/Struct/Frustum.h"
#include <DirectXMath.h>

namespace DX12Engine {

namespace Math {
using BoundingVolumeVariant = Math::BoundingVolumeVariant;
}

namespace Renderer {

class CameraManager;

// ============================================================================
// 剔除结果
// ============================================================================
struct CullingResult {
    std::vector<ECS::Entity> visibleEntities;

    void Clear() { visibleEntities.clear(); }
    void SetVisible(ECS::Entity entity) { visibleEntities.push_back(entity); }
    bool IsVisible(ECS::Entity entity) const {
        // 线性查找（小规模没问题）
        return std::find(visibleEntities.begin(), visibleEntities.end(), entity) != visibleEntities.end();
    }
    size_t Size() const { return visibleEntities.size(); }
};

// ============================================================================
// 剔除系统 - 负责计算实体的可见性
// ============================================================================
class CullingSystem {
public:
    CullingSystem() = default;
    ~CullingSystem() = default;

    // 禁止拷贝
    CullingSystem(const CullingSystem &) = delete;
    CullingSystem &operator=(const CullingSystem &) = delete;

    void SetFrustum(const Frustum *frustum) { m_frustum = frustum; }

    /// 设置相机管理器引用（用于获取相机位置/速度进行预测剔除）
    void SetCameraManager(CameraManager *mgr) { m_cameraManager = mgr; }

    /// 设置预测安全系数（默认 1.3，范围推荐 1.0~1.5）
    void SetPredictionFactor(float factor) { m_predictionFactor = factor; }

    void Execute(ECS::Registry &registry, CullingResult &outResult);

    /// 使用预测位置构建视锥体（相机运动补偿）
    /// @param dt 帧间隔（秒）
    Frustum BuildPredictedFrustum(float dt) const;

private:
    bool Intersects(const Frustum &frustum, const Math::BoundingAABB &bounds) const;
    bool Intersects(const Frustum &frustum, const Math::BoundingSphere &bounds) const;
    bool TestVisibility(const Frustum &frustum, const Math::BoundingVolumeVariant &bounds,
                        const DirectX::XMFLOAT3 &worldPosition) const;

    const Frustum *m_frustum = nullptr;
    CameraManager *m_cameraManager = nullptr;
    float m_predictionFactor = 1.3f; // 安全系数 K
};

} // namespace Renderer

} // namespace DX12Engine