#pragma once

#include "CulledSet.h"
#include "ECS/Core/Entity.h"
#include "ECS/Core/Registry.h"
#include "Math/MathTypes.h"
#include "Renderer/Scene/CameraManager.h"
#include "Renderer/Scene/Struct/Frustum.h"
#include <vector>

namespace DX12Engine {

namespace Renderer {

struct PredictedCameraData;

// ============================================================================
// 射线命中结果
// ============================================================================
struct RaycastHit {
    ECS::Entity entity = ECS::INVALID_ENTITY;
    float distance = 0.0f;
    XMFLOAT3 hitPoint = {0.0f, 0.0f, 0.0f};
};

// ============================================================================
// 射线检测结果（多个命中，按距离排序：近→远）
// ============================================================================
struct RaycastResult {
    std::vector<RaycastHit> hits;

    bool HasAny() const { return !hits.empty(); }
    const RaycastHit &GetClosest() const { return hits[0]; }
    const RaycastHit &GetFarthest() const { return hits.back(); }
};

// ============================================================================
// VisibleRaycaster — 基于可见集/ECS 的射线检测工具（纯引擎层）
//
// 职责：
//   - 屏幕坐标 → 世界射线
//   - 在可见集（CulledSet）上做精确射线相交测试
//   - 返回所有命中实体列表（按距离排序）
//
// 使用方式：
//   RaycastOnSet(ray, culledSet, registry)：在可见集上检测（推荐，Editor/Game 共用）
//   RaycastAll(ray)：在全部 Registry 上检测（旧接口，保留兼容）
//
// 不负责：输入状态检测、上下文过滤、结果应用（高亮/选中）
// ============================================================================
class VisibleRaycaster {
public:
    VisibleRaycaster() = default;
    ~VisibleRaycaster() = default;

    VisibleRaycaster(const VisibleRaycaster &) = delete;
    VisibleRaycaster &operator=(const VisibleRaycaster &) = delete;

    void Initialize(ECS::Registry *registry);

    // 设置相机数据（用于 ScreenToRay）
    void UpdateCameraData(const PredictedCameraData &cameraData) { m_cameraData = cameraData; }

    // 从屏幕坐标生成世界空间射线
    FRay ScreenToRay(float screenX, float screenY, uint32_t screenWidth, uint32_t screenHeight) const;

    // ========================================================================
    // 数据源：可见集（推荐路径）
    // ========================================================================

    /// 在可见集上做射线检测（Editor/Game 共用）
    /// @param ray 世界空间射线
    /// @param culledSet 剔除管线输出的可见集
    /// @param registry ECS Registry（用于获取精确包围盒和 Transform）
    /// @return 命中结果（按距离由近到远排序）
    RaycastResult RaycastOnSet(const FRay &ray, const CulledSet &culledSet, ECS::Registry *registry) const;

    // ========================================================================
    // 数据源：全 Registry 遍历（旧路径，保留兼容）
    // ========================================================================

    /// 对所有 MeshComponent 实体做射线检测，返回命中（按距离由近到远排序）
    RaycastResult RaycastAll(const FRay &ray) const;

private:
    void CollectHits(const FRay &ray, std::vector<RaycastHit> &outHits) const;

    /// 在给定实体列表上做精确射线相交测试
    void CollectHitsOnEntities(const FRay &ray, const std::vector<ECS::Entity> &entities,
                                ECS::Registry *registry, std::vector<RaycastHit> &outHits) const;

    ECS::Registry *m_registry = nullptr;
    PredictedCameraData m_cameraData{};
};

} // namespace Renderer
} // namespace DX12Engine
