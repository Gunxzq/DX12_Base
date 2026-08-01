#pragma once

#include "CulledSet.h"
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
struct PredictedCameraData;

// ============================================================================
// 剔除系统 — 维护双视锥体（剔除 + 渲染）+ 可见集
//
// 职责分层：
//   PreCulling（OctreeSystem）：八叉树粗筛 → CulledSet（候选集）
//   PostCulling（CullingSystem）：视锥精筛 + 场景过滤 → CulledSet（可见集）
// ============================================================================
class CullingSystem {
public:
    CullingSystem() = default;
    ~CullingSystem() = default;

    CullingSystem(const CullingSystem &) = delete;
    CullingSystem &operator=(const CullingSystem &) = delete;

    void SetCamera(const PredictedCameraData &camera);

    /// 获取剔除视锥体（宽远平面，供 Builder/CullingUtil 做 CPU 剔除）
    const Frustum &GetFrustum() const { return m_cullFrustum; }
    bool HasFrustum() const { return m_hasCullFrustum; }

    /// 获取渲染视锥体（紧远平面，用于投影矩阵/SSAO/Shadow Map 空间计算）
    const Frustum &GetRenderFrustum() const { return m_renderFrustum; }

    // ========================================================================
    // 可见集管线
    // ========================================================================

    /// 接受八叉树粗筛候选集，做精确视锥剔除和场景过滤
    /// @param candidates OctreeSystem 输出的候选集
    /// @param activeSceneId 当前活跃场景 ID（Editor 端传入，Game 端传 0）
    /// @param outVisible 输出可见集（视锥内 + 场景匹配的实体）
    void Cull(const CulledSet &candidates, uint64_t activeSceneId, CulledSet &outVisible) const;

private:
    Frustum m_cullFrustum;    // 剔除视锥（宽范围）
    Frustum m_renderFrustum;  // 渲染视锥（紧范围）
    bool m_hasCullFrustum = false;
};

} // namespace Renderer
} // namespace DX12Engine
