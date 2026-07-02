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
// 剔除系统 — 仅提供视锥体维护（供 VisibleRaycaster 等使用）
// ============================================================================
class CullingSystem {
public:
    CullingSystem() = default;
    ~CullingSystem() = default;

    CullingSystem(const CullingSystem &) = delete;
    CullingSystem &operator=(const CullingSystem &) = delete;

    void SetCamera(const PredictedCameraData &camera);

    /// 获取当前视锥体（供 Builder/CullingUtil 使用）
    const Frustum &GetFrustum() const { return m_frustum; }
    bool HasFrustum() const { return m_hasFrustum; }

private:
    Frustum m_frustum;
    bool m_hasFrustum = false;
};

} // namespace Renderer
} // namespace DX12Engine
