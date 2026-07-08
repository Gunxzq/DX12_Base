#pragma once

#include "ECS/Core/Registry.h"
#include "IRenderItemBuilder.h"
#include "Renderer/Core/CullingUtil.h"
#include "Renderer/Core/LODSystem.h"
#include "Renderer/Scene/Struct/Frustum.h"
#include "TRenderQueue.h"
#include "WaterRenderItem.h"

namespace DX12Engine::Resource {
class MaterialManager;
class TextureManager;
} // namespace DX12Engine::Resource

namespace DX12Engine::Renderer {

class CameraManager;
class FrameResourceManager;

// ============================================================================
// WaterRenderItemBuilder — 水体构建器
//
// PreRender 阶段扫描 WaterComponent + MeshComponent + TransformComponent，
// 产出 WaterRenderItem。ObjectConstants CB 在 FrameSync 中分配。
// ============================================================================
class WaterRenderItemBuilder : public TRenderItemBuilder<TRenderQueue<WaterRenderItem>> {
public:
    WaterRenderItemBuilder(FrameResourceManager *frameResources, Resource::MaterialManager *materialManager,
                           CameraManager *cameraManager);

    void SetFrustum(const Frustum *frustum) { m_frustum = frustum; }
    void SetCameraPos(const DirectX::XMFLOAT3 &pos) { m_cameraPos = pos; }
    void SetLODSystem(const LODSystem *system) { m_lodSystem = system; }

    void BuildTyped(ECS::Registry &registry, TRenderQueue<WaterRenderItem> &outQueue) override;

    const char *GetName() const override { return "WaterRenderItemBuilder"; }

private:
    FrameResourceManager *m_frameResourceManager;
    Resource::MaterialManager *m_materialManager;
    CameraManager *m_cameraManager;

    const Frustum *m_frustum = nullptr;
    const LODSystem *m_lodSystem = nullptr;
    DirectX::XMFLOAT3 m_cameraPos = {};
};

} // namespace DX12Engine::Renderer
