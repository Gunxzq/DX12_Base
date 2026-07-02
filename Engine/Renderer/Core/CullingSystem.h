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
struct PredictedCameraData;

// ============================================================================
// 剔除系统 — 维护双视锥体（剔除 + 渲染）
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

private:
    Frustum m_cullFrustum;    // 剔除视锥（宽范围）
    Frustum m_renderFrustum;  // 渲染视锥（紧范围）
    bool m_hasCullFrustum = false;
};

} // namespace Renderer
} // namespace DX12Engine
