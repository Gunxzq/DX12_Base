#include "CullingSystem.h"

#include "Renderer/Scene/CameraManager.h"

namespace DX12Engine::Renderer {

void CullingSystem::SetCamera(const PredictedCameraData &camera) {
    // 构建剔除视锥（宽远平面：CullFarPlane）
    m_cullFrustum.BuildFromCamera(camera.Position, camera.Forward, camera.Up, camera.FOV, camera.AspectRatio,
                                  camera.NearPlane, camera.CullFarPlane);
    m_hasCullFrustum = true;

    // 构建渲染视锥（紧远平面：FarPlane）
    m_renderFrustum.BuildFromCamera(camera.Position, camera.Forward, camera.Up, camera.FOV, camera.AspectRatio,
                                    camera.NearPlane, camera.FarPlane);
}

} // namespace DX12Engine::Renderer
