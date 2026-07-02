#include "CullingSystem.h"

#include "Renderer/Scene/CameraManager.h"

namespace DX12Engine::Renderer {

void CullingSystem::SetCamera(const PredictedCameraData &camera) {
    m_frustum.BuildFromCamera(camera.Position, camera.Forward, camera.Up, camera.FOV, camera.AspectRatio,
                              camera.NearPlane, camera.FarPlane);
    m_hasFrustum = true;
}

} // namespace DX12Engine::Renderer
