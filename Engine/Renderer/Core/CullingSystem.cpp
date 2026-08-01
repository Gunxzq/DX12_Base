#include "CullingSystem.h"

#include "Logger/Logger.h"
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

void CullingSystem::Cull(const CulledSet &candidates, uint64_t activeSceneId, CulledSet &outVisible) const {
    outVisible.Clear();

    if (!m_hasCullFrustum) {
        // 无视锥体时直接输出全部候选（兜底行为）
        outVisible = candidates;
        return;
    }

    size_t beforeCount = candidates.Size();
    size_t afterCount = 0;
    size_t sceneFiltered = 0;
    size_t frustumFiltered = 0;

    for (const auto &entry : candidates.entries) {
        // 场景过滤：Editor 端只保留活跃场景实体，Game 端 sceneId=0 全部通过
        if (activeSceneId != 0 && entry.sceneId != 0 && entry.sceneId != activeSceneId) {
            ++sceneFiltered;
            continue;
        }

        // 精确视锥剔除
        if (m_cullFrustum.Intersects(entry.worldBounds)) {
            outVisible.Add(entry.entity, entry.worldBounds, entry.sceneId);
            ++afterCount;
        } else {
            ++frustumFiltered;
        }
    }

    if (beforeCount > 0) {
        // auto *logger = Logger::Logger::GetInstance();
        // logger->Info("[CullingSystem::Cull] before={}, after={}, sceneFiltered={}, frustumFiltered={}",
        //              beforeCount, afterCount, sceneFiltered, frustumFiltered);
    }
}

} // namespace DX12Engine::Renderer
