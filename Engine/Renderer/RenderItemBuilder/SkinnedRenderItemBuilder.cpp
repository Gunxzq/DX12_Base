#include "SkinnedRenderItemBuilder.h"
#include "ECS/Core/Components.h"
#include "Renderer/FrameResources/FrameResourceManager.h"
#include "Resource/Manager/MaterialManager.h"
#include "Resource/Manager/SkeletonManager.h"

using namespace DX12Engine::Renderer;
using namespace DX12Engine::Resource;
using namespace DX12Engine::ECS;

namespace DX12Engine::Renderer {

// ============================================================================
// 分组键：同 Mesh + 同 Material + 同子网格偏移可以合批
// ============================================================================
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

void SkinnedRenderItemBuilder::BuildTyped(ECS::Registry &registry, TRenderQueue<SkinnedRenderItem> &outQueue) {
    outQueue.Clear();

    if (!m_cullingResult || !m_lodResult) {
        return;
    }

    // ========================================================================
    // 批处理：遍历所有同时持有 MeshComponent + SkinnedComponent 的可见实体
    // ========================================================================
    struct BatchEntry {
        std::vector<InstanceData> instances;
        std::vector<Entity> entities;
    };
    std::unordered_map<SkinnedBatchKey, BatchEntry, SkinnedBatchKeyHash> batches;

    for (auto entity : m_cullingResult->visibleEntities) {
        // 只有带 SkinnedTag 的实体才走此 Builder
        if (!registry.HasComponent<SkinnedTag>(entity))
            continue;

        auto *meshComp = registry.TryGetComponent<MeshComponent>(entity);
        if (!meshComp)
            continue;

        GeometryHandle geoHandle = m_lodResult->GetHandle(entity);
        if (!geoHandle.IsValid())
            continue;

        auto *transform = registry.TryGetComponent<TransformComponent>(entity);
        if (!transform)
            continue;

        uint32_t materialIdx = m_materialManager->GetGPUIndex(meshComp->materialHandle);

        // --- InstanceData（与 Opaque 相同，World 矩阵来自 Transform） ---
        InstanceData instData = {};
        XMMATRIX world = transform->GetMatrix();
        XMMATRIX worldInvTranspose = XMMatrixTranspose(XMMatrixInverse(nullptr, world));
        XMStoreFloat4x4(&instData.World, world);
        XMStoreFloat4x4(&instData.WorldInvTranspose, worldInvTranspose);
        instData.MaterialIndex = materialIdx;
        instData.ReceiveShadow = meshComp->receivesShadow ? 1 : 0;
        // 蒙皮角色暂不考虑反射探针
        instData.ProbeIndex = UINT32_MAX;

        SkinnedBatchKey key{geoHandle, materialIdx, meshComp->indexCount, meshComp->startIndex, meshComp->startVertex};
        batches[key].instances.push_back(instData);
        batches[key].entities.push_back(entity);
    }

    // ========================================================================
    // 每批次：上传 InstanceData + 骨骼矩阵 → 产出 SkinnedRenderItem
    // ========================================================================
    for (auto &[key, entry] : batches) {
        auto &instances = entry.instances;

        // 上传 InstanceData
        D3D12_GPU_VIRTUAL_ADDRESS instanceBuffer = m_frameResourceManager->AllocateInstance(
            instances.data(), static_cast<uint32_t>(instances.size() * sizeof(InstanceData)));

        // 骨骼矩阵由 AnimationAdvancer 在 LateUpdate 阶段预计算并上传，
        // 此处直接从 SkinnedComponent.boneBufferAddress 读取 GPU 地址
        D3D12_GPU_VIRTUAL_ADDRESS boneBuffer = 0;

        if (!entry.entities.empty()) {
            Entity firstEntity = entry.entities.front();
            auto *skinnedComp = registry.TryGetComponent<SkinnedComponent>(firstEntity);
            if (skinnedComp && skinnedComp->boneBufferAddress != 0) {
                boneBuffer = skinnedComp->boneBufferAddress;
            }
        }

        if (boneBuffer == 0) {
            // 骨骼上传失败时跳过该批次
            continue;
        }

        SkinnedRenderItem item = SkinnedRenderItem::Create(key.geometry, key.materialIdx, instanceBuffer, boneBuffer,
                                                           static_cast<uint32_t>(instances.size()), key.indexCount,
                                                           key.startIndex, key.startVertex);
        outQueue.Add(item);
    }
}

} // namespace DX12Engine::Renderer
