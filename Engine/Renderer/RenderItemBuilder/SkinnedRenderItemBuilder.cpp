#include "SkinnedRenderItemBuilder.h"
#include "ECS/Core/Components.h"
#include "Renderer/FrameResources/FrameResourceManager.h"
#include "Renderer/Material/MaterialManager.h"
#include "Resource/Manager/SkeletonManager.h"

using namespace DX12Engine::Renderer;
using namespace DX12Engine::Resource;
using namespace DX12Engine::ECS;

namespace DX12Engine::Renderer {

struct SkinnedBatchKey {
    GeometryHandle geometry;
    uint32_t materialIdx;
    uint32_t indexCount = 0;
    uint32_t startIndex = 0;
    int32_t startVertex = 0;

    bool operator==(const SkinnedBatchKey &other) const {
        return geometry.index == other.geometry.index && materialIdx == other.materialIdx &&
               indexCount == other.indexCount && startIndex == other.startIndex && startVertex == other.startVertex;
    }
};

struct SkinnedBatchKeyHash {
    size_t operator()(const SkinnedBatchKey &key) const {
        return ((size_t)key.geometry.index << 32) ^ (key.materialIdx << 1) ^ ((size_t)key.indexCount << 2) ^
               ((size_t)key.startIndex << 16) ^ ((size_t)(uint32_t)key.startVertex);
    }
};

SkinnedRenderItemBuilder::SkinnedRenderItemBuilder(FrameResourceManager *frameResources,
                                                   MaterialManager *materialManager, SkeletonManager *skeletonManager)
    : m_frameResourceManager(frameResources), m_materialManager(materialManager), m_skeletonManager(skeletonManager) {}

uint32_t SkinnedRenderItemBuilder::Count(ECS::Registry &registry) {
    if (!m_frustum)
        return 0;

    uint32_t count = 0;
    auto view = registry.view<MeshComponent, TransformComponent, SkinnedTag>();
    for (auto entity : view) {
        auto &meshComp = view.get<MeshComponent>(entity);
        auto &transform = view.get<TransformComponent>(entity);

        if (!FrustumCull(meshComp.localBounds, transform.GetMatrix(), *m_frustum))
            continue;

        GeometryHandle geoHandle;
        if (m_lodSystem && meshComp.lodMeshHandle.IsValid()) {
            const auto *lodMesh = m_lodSystem->GetLODMesh(meshComp.lodMeshHandle);
            if (lodMesh) {
                geoHandle = PickLOD(*lodMesh, transform.position, m_cameraPos, m_lodSystem->GetLODConfig());
            }
        }
        if (!geoHandle.IsValid())
            continue;

        count++;
    }
    return count;
}

void SkinnedRenderItemBuilder::BuildTyped(ECS::Registry &registry, TRenderQueue<SkinnedRenderItem> &outQueue) {
    outQueue.Clear();

    if (!m_frustum)
        return;

    struct BatchEntry {
        std::vector<InstanceData> instances;
        std::vector<Entity> entities;
    };
    std::unordered_map<SkinnedBatchKey, BatchEntry, SkinnedBatchKeyHash> batches;

    auto view = registry.view<MeshComponent, TransformComponent, SkinnedTag>();
    for (auto entity : view) {
        auto &meshComp = view.get<MeshComponent>(entity);
        auto &transform = view.get<TransformComponent>(entity);

        if (!FrustumCull(meshComp.localBounds, transform.GetMatrix(), *m_frustum))
            continue;

        GeometryHandle geoHandle;
        if (m_lodSystem && meshComp.lodMeshHandle.IsValid()) {
            const auto *lodMesh = m_lodSystem->GetLODMesh(meshComp.lodMeshHandle);
            if (lodMesh) {
                geoHandle = PickLOD(*lodMesh, transform.position, m_cameraPos, m_lodSystem->GetLODConfig());
            }
        }
        if (!geoHandle.IsValid())
            continue;

        uint32_t materialIdx = m_materialManager->GetGPUIndex(
            meshComp.materialSlots.empty() ? Resource::MaterialHandle::Invalid() : meshComp.materialSlots[0]);

        InstanceData instData = {};
        XMMATRIX world = transform.GetMatrix();
        XMMATRIX worldInvTranspose = XMMatrixTranspose(XMMatrixInverse(nullptr, world));
        XMStoreFloat4x4(&instData.World, world);
        XMStoreFloat4x4(&instData.WorldInvTranspose, worldInvTranspose);
        instData.MaterialIndex = materialIdx;
        instData.ReceiveShadow = meshComp.receivesShadow ? 1 : 0;
        instData.ProbeIndex = UINT32_MAX;

        SkinnedBatchKey key{geoHandle, materialIdx, 0, 0, 0}; // SubMesh 展开在 Step 4 中实现
        auto &entry = batches[key];
        entry.instances.push_back(instData);
        entry.entities.push_back(entity);
    }

    // 写入临时批次（FrameSync 统一上传用）
    m_pendingBatches.clear();
    for (auto &[key, entry] : batches) {
        auto &instances = entry.instances;
        if (instances.empty())
            continue;

        uint32_t qIdx = static_cast<uint32_t>(outQueue.Size());
        PendingBatch pendingBatch;
        pendingBatch.instances = std::move(instances);
        pendingBatch.entities = std::move(entry.entities);
        pendingBatch.queueIndex = qIdx;
        m_pendingBatches.push_back(std::move(pendingBatch));

        D3D12_GPU_VIRTUAL_ADDRESS boneBuffer = 0;
        auto &pending = m_pendingBatches.back();
        if (!pending.entities.empty()) {
            Entity firstEntity = pending.entities.front();
            auto *skinnedComp = registry.TryGetComponent<SkinnedComponent>(firstEntity);
            if (skinnedComp && skinnedComp->boneBufferAddress != 0)
                boneBuffer = skinnedComp->boneBufferAddress;
        }
        if (boneBuffer == 0)
            continue;

        uint32_t slot = static_cast<uint32_t>(m_pendingBatches.size() - 1);
        // startVertex 恒 0（当前 key 全 0）：dxmesh 索引已绝对化，BaseVertexLocation 必须为 0（见
        // SubMeshMaterialSlots.md §2.3）
        SkinnedRenderItem item = SkinnedRenderItem::Create(key.geometry, key.materialIdx, 0, boneBuffer,
                                                           static_cast<uint32_t>(pending.instances.size()),
                                                           key.indexCount, key.startIndex, key.startVertex);
        item.tempSlot = slot;
        outQueue.Add(item);
    }
}

} // namespace DX12Engine::Renderer
