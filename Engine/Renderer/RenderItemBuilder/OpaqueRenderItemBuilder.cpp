#include "OpaqueRenderItemBuilder.h"
#include "ECS/Core/Components.h"
#include "Renderer/FrameResources/FrameResourceManager.h"
#include "Renderer/Material/MaterialManager.h"
#include "Resource/Texture/TextureManager.h"

using namespace DX12Engine::Renderer;
using namespace DX12Engine::Resource;
using namespace DX12Engine::ECS;

namespace DX12Engine::Renderer {

// 分组键：相同 Mesh + 相同 Material 的实体可以合批
struct BatchKey {
    GeometryHandle geometry;
    uint32_t materialIdx;
    uint32_t startIndex = 0;
    int32_t startVertex = 0;
    uint32_t indexCount = 0;

    bool operator==(const BatchKey &other) const {
        return geometry.index == other.geometry.index && materialIdx == other.materialIdx &&
               startIndex == other.startIndex && startVertex == other.startVertex && indexCount == other.indexCount;
    }
};

struct BatchKeyHash {
    size_t operator()(const BatchKey &key) const {
        return ((size_t)key.geometry.index << 32) ^ (key.materialIdx << 1) ^ ((size_t)key.startIndex << 16) ^
               ((size_t)key.startVertex) ^ ((size_t)key.indexCount << 2);
    }
};

OpaqueRenderItemBuilder::OpaqueRenderItemBuilder(FrameResourceManager *frameResources, MaterialManager *materialManager,
                                                 TextureManager *textureManager)
    : m_frameResourceManager(frameResources), m_materialManager(materialManager), m_textureManager(textureManager) {}

// ========================================================================
// Count — 轻量计数遍（只做剔除+LOD判断，不分配不构建）
// ========================================================================

uint32_t OpaqueRenderItemBuilder::Count(ECS::Registry &registry) {
    if (!m_frustum)
        return 0;

    uint32_t count = 0;
    auto view = registry.view<MeshComponent, TransformComponent, OpaqueTag>();
    for (auto entity : view) {
        // 可选实体过滤器（编辑器端用于按 SceneTagComponent 过滤）
        if (m_entityFilter && !m_entityFilter(entity))
            continue;

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

// ========================================================================
// BuildTyped — 实际构建
// ========================================================================

void OpaqueRenderItemBuilder::BuildTyped(ECS::Registry &registry, TRenderQueue<OpaqueRenderItem> &outQueue) {
    outQueue.Clear();

    if (!m_frustum)
        return;

    struct BatchEntry {
        std::vector<InstanceData> instances;
        std::vector<Entity> entities;
        uint32_t probeIndex = UINT32_MAX;
    };
    std::unordered_map<BatchKey, BatchEntry, BatchKeyHash> batches;

    auto view = registry.view<MeshComponent, TransformComponent, OpaqueTag>();
    for (auto entity : view) {
        // 可选实体过滤器（编辑器端用于按 SceneTagComponent 过滤）
        if (m_entityFilter && !m_entityFilter(entity))
            continue;

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

        uint32_t materialIdx = m_materialManager->GetGPUIndex(meshComp.materialHandle);

        InstanceData instData = {};
        XMMATRIX world = transform.GetMatrix();
        XMMATRIX worldInvTranspose = XMMatrixTranspose(XMMatrixInverse(nullptr, world));
        XMStoreFloat4x4(&instData.World, world);
        XMStoreFloat4x4(&instData.WorldInvTranspose, worldInvTranspose);
        instData.MaterialIndex = materialIdx;
        instData.ReceiveShadow = meshComp.receivesShadow ? 1 : 0;

        uint32_t probeIdx = UINT32_MAX;
        auto *reflectionComp = registry.TryGetComponent<ReflectionConsumerComponent>(entity);
        if (reflectionComp)
            probeIdx = reflectionComp->probeIndex;
        instData.ProbeIndex = probeIdx;

        BatchKey key{geoHandle, materialIdx, meshComp.startIndex, meshComp.startVertex, meshComp.indexCount};
        auto &entry = batches[key];
        entry.instances.push_back(instData);
        entry.probeIndex = probeIdx;
    }

    for (auto &[key, entry] : batches) {
        auto &instances = entry.instances;
        if (instances.empty())
            continue;

        uint32_t queueIndex = static_cast<uint32_t>(outQueue.Size());
        uint32_t instCount = static_cast<uint32_t>(instances.size());
        m_pendingBatches.push_back({std::move(instances), queueIndex});

        OpaqueRenderItem item =
            OpaqueRenderItem::Create(key.geometry, key.materialIdx, 0, instCount,
                                     entry.probeIndex, key.startIndex, key.startVertex, key.indexCount);
        item.tempSlot = static_cast<uint32_t>(m_pendingBatches.size() - 1);
        outQueue.Add(item);
    }
}

} // namespace DX12Engine::Renderer
