#pragma once

#include "ECS/Core/Registry.h"
#include "IRenderItemBuilder.h"
#include "OpaqueRenderItem.h"
#include "Renderer/Core/CullingUtil.h"
#include "Renderer/Core/LODSystem.h"
#include "Renderer/Scene/Struct/Frustum.h"
#include "TRenderQueue.h"

// 前向声明
namespace DX12Engine::Resource {
class MaterialManager;
class TextureManager;
} // namespace DX12Engine::Resource

namespace DX12Engine::Renderer {

class FrameResourceManager;

class OpaqueRenderItemBuilder : public TRenderItemBuilder<TRenderQueue<OpaqueRenderItem>> {
public:
    OpaqueRenderItemBuilder(FrameResourceManager *frameResources, Resource::MaterialManager *materialManager,
                            Resource::TextureManager *textureManager);

    void SetFrustum(const Frustum *frustum) { m_frustum = frustum; }
    void SetCameraPos(const DirectX::XMFLOAT3 &pos) { m_cameraPos = pos; }
    void SetLODSystem(const LODSystem *system) { m_lodSystem = system; }

    // 临时批次数据（FrameSync 统一上传用）
    struct PendingBatch {
        std::vector<InstanceData> instances;
        uint32_t queueIndex;
    };
    std::vector<PendingBatch> &GetPendingBatches() { return m_pendingBatches; }

    uint32_t Count(ECS::Registry &registry);

    void BuildTyped(ECS::Registry &registry, TRenderQueue<OpaqueRenderItem> &outQueue) override;

private:
    FrameResourceManager *m_frameResourceManager;
    Resource::MaterialManager *m_materialManager;
    Resource::TextureManager *m_textureManager;

    const Frustum *m_frustum = nullptr;
    const LODSystem *m_lodSystem = nullptr;
    DirectX::XMFLOAT3 m_cameraPos = {};
    std::vector<PendingBatch> m_pendingBatches;
};

} // namespace DX12Engine::Renderer
