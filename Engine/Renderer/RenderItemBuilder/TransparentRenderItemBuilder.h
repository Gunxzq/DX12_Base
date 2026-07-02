#pragma once

#include "ECS/Core/Registry.h"
#include "Renderer/FrameResources/Struct/FrameResourceTypes.h"
#include "IRenderItemBuilder.h"
#include "Renderer/Core/CullingUtil.h"
#include "Renderer/Core/LODSystem.h"
#include "Renderer/Scene/Struct/Frustum.h"
#include "TRenderQueue.h"
#include "TransparentRenderItem.h"

namespace DX12Engine::Resource {
class MaterialManager;
class TextureManager;
} // namespace DX12Engine::Resource

namespace DX12Engine::Renderer {

class CameraManager;
class FrameResourceManager;

class TransparentRenderItemBuilder : public TRenderItemBuilder<TRenderQueue<TransparentRenderItem>> {
public:
    TransparentRenderItemBuilder(FrameResourceManager *frameResources, Resource::MaterialManager *materialManager,
                                 Resource::TextureManager *textureManager, CameraManager *cameraManager);

    void SetFrustum(const Frustum *frustum) { m_frustum = frustum; }
    void SetCameraPos(const DirectX::XMFLOAT3 &pos) { m_cameraPos = pos; }
    void SetLODSystem(const LODSystem *system) { m_lodSystem = system; }

    /// 临时 ObjectConstants（FrameSync 统一上传用）
    struct PendingBatch {
        ObjectConstants object;
    };
    std::vector<PendingBatch> &GetPendingBatches() { return m_pendingBatches; }

    uint32_t Count(ECS::Registry &registry);

    void BuildTyped(ECS::Registry &registry, TRenderQueue<TransparentRenderItem> &outQueue) override;

private:
    float CalculateDepth(const DirectX::XMFLOAT3 &pos, const DirectX::XMFLOAT3 &cameraPos) const;

    FrameResourceManager *m_frameResourceManager;
    Resource::MaterialManager *m_materialManager;
    Resource::TextureManager *m_textureManager;
    CameraManager *m_cameraManager;

    const Frustum *m_frustum = nullptr;
    const LODSystem *m_lodSystem = nullptr;
    DirectX::XMFLOAT3 m_cameraPos = {};
    std::vector<PendingBatch> m_pendingBatches;
};

} // namespace DX12Engine::Renderer
