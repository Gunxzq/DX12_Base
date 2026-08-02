#include "TransparentRenderItemBuilder.h"
#include "ECS/Core/Components.h"
#include "Renderer/FrameResources/FrameResourceManager.h"
#include "Renderer/Material/MaterialManager.h"
#include "Renderer/Scene/CameraManager.h"
#include "Resource/Texture/TextureManager.h"

using namespace DX12Engine::Renderer;
using namespace DX12Engine::Resource;
using namespace DX12Engine::ECS;

namespace DX12Engine::Renderer {

TransparentRenderItemBuilder::TransparentRenderItemBuilder(FrameResourceManager *frameResources,
                                                           MaterialManager *materialManager,
                                                           TextureManager *textureManager, CameraManager *cameraManager)
    : m_frameResourceManager(frameResources), m_materialManager(materialManager), m_textureManager(textureManager),
      m_cameraManager(cameraManager) {}

uint32_t TransparentRenderItemBuilder::Count(ECS::Registry &registry) {
    if (!m_frustum)
        return 0;

    uint32_t count = 0;
    auto view = registry.view<MeshComponent, TransformComponent, TransparentTag>();
    for (auto entity : view) {
        auto &comp = view.get<MeshComponent>(entity);
        auto &transform = view.get<TransformComponent>(entity);

        if (!FrustumCull(comp.localBounds, transform.GetMatrix(), *m_frustum))
            continue;

        GeometryHandle geoHandle;
        if (m_lodSystem && comp.lodMeshHandle.IsValid()) {
            const auto *lodMesh = m_lodSystem->GetLODMesh(comp.lodMeshHandle);
            if (lodMesh)
                geoHandle = PickLOD(*lodMesh, transform.position, m_cameraPos, m_lodSystem->GetLODConfig());
        }
        if (!geoHandle.IsValid() || comp.materialSlots.empty() || !comp.materialSlots[0].IsValid())
            continue;

        count++;
    }
    return count;
}

void TransparentRenderItemBuilder::BuildTyped(ECS::Registry &registry, TRenderQueue<TransparentRenderItem> &outQueue) {
    outQueue.Clear();
    m_pendingBatches.clear();

    if (!m_frustum)
        return;

    auto view = registry.view<MeshComponent, TransformComponent, TransparentTag>();
    for (auto entity : view) {
        auto &comp = view.get<MeshComponent>(entity);
        auto &transform = view.get<TransformComponent>(entity);

        if (!FrustumCull(comp.localBounds, transform.GetMatrix(), *m_frustum))
            continue;

        GeometryHandle geoHandle;
        if (m_lodSystem && comp.lodMeshHandle.IsValid()) {
            const auto *lodMesh = m_lodSystem->GetLODMesh(comp.lodMeshHandle);
            if (lodMesh)
                geoHandle = PickLOD(*lodMesh, transform.position, m_cameraPos, m_lodSystem->GetLODConfig());
        }
        if (!geoHandle.IsValid())
            continue;

        MaterialHandle materialHandle = comp.materialSlots.empty() ? MaterialHandle::Invalid() : comp.materialSlots[0];
        if (!materialHandle.IsValid())
            continue;

        float depth = CalculateDepth(transform.position, m_cameraPos);

        ObjectConstants objCB;
        XMMATRIX world = transform.GetMatrix();
        XMMATRIX worldInvTranspose = XMMatrixTranspose(XMMatrixInverse(nullptr, world));
        XMStoreFloat4x4(&objCB.World, world);
        XMStoreFloat4x4(&objCB.WorldInvTranspose, worldInvTranspose);
        objCB.MaterialIndex = m_materialManager->GetGPUIndex(materialHandle);
        objCB.ReceiveShadow = 0;

        // 暂存 ObjectConstants 到 PendingBatch，FrameSync 统一上传
        auto &batch = m_pendingBatches.emplace_back();
        batch.object = objCB;
        uint32_t batchIdx = static_cast<uint32_t>(m_pendingBatches.size() - 1);

        TransparentRenderItem item;
        item.geometryHandle = geoHandle;
        item.worldMatrix = world;
        item.objectCBAddress = 0; // FrameSync 回填
        item.materialIndex = objCB.MaterialIndex;
        item.depth = depth;
        item.tempSlot = batchIdx;

        outQueue.Add(item);
    }

    outQueue.Sort([](const TransparentRenderItem &a, const TransparentRenderItem &b) { return a.depth > b.depth; });
}

float TransparentRenderItemBuilder::CalculateDepth(const DirectX::XMFLOAT3 &pos,
                                                   const DirectX::XMFLOAT3 &cameraPos) const {
    float dx = pos.x - cameraPos.x;
    float dy = pos.y - cameraPos.y;
    float dz = pos.z - cameraPos.z;
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

} // namespace DX12Engine::Renderer
