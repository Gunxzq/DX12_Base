#pragma once

#include "ECS/Core/Entity.h"
#include "ECS/Core/Registry.h"
#include "IRenderItemBuilder.h"
#include "Renderer/Core/CullingUtil.h"
#include "Renderer/Core/LODSystem.h"
#include "Renderer/Scene/Struct/Frustum.h"
#include "SkinnedRenderItem.h"
#include "TRenderQueue.h"

#include <vector>

namespace DX12Engine::Resource {
class MaterialManager;
class SkeletonManager;
} // namespace DX12Engine::Resource

namespace DX12Engine::Renderer {

class FrameResourceManager;

class SkinnedRenderItemBuilder : public TRenderItemBuilder<TRenderQueue<SkinnedRenderItem>> {
public:
    SkinnedRenderItemBuilder(FrameResourceManager *frameResources, Resource::MaterialManager *materialManager,
                             Resource::SkeletonManager *skeletonManager);

    void SetFrustum(const Frustum *frustum) { m_frustum = frustum; }
    void SetCameraPos(const DirectX::XMFLOAT3 &pos) { m_cameraPos = pos; }
    void SetLODSystem(const LODSystem *system) { m_lodSystem = system; }

    struct PendingBatch {
        std::vector<InstanceData> instances;
        std::vector<ECS::Entity> entities;
        uint32_t queueIndex;
    };
    std::vector<PendingBatch> &GetPendingBatches() { return m_pendingBatches; }

    uint32_t Count(ECS::Registry &registry);

    void BuildTyped(ECS::Registry &registry, TRenderQueue<SkinnedRenderItem> &outQueue) override;

private:
    FrameResourceManager *m_frameResourceManager;
    Resource::MaterialManager *m_materialManager;
    Resource::SkeletonManager *m_skeletonManager;

    const Frustum *m_frustum = nullptr;
    const LODSystem *m_lodSystem = nullptr;
    DirectX::XMFLOAT3 m_cameraPos = {};
    std::vector<DirectX::XMFLOAT4X4> m_boneTransformCache;

    std::vector<PendingBatch> m_pendingBatches;
};

} // namespace DX12Engine::Renderer
