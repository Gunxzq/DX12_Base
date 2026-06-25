#include "OpaqueRenderItemBuilder.h"
#include "ECS/Core/Components.h"
#include "Renderer/FrameResources/FrameResourceManager.h"
#include "Resource/Manager/MaterialManager.h"
#include "Resource/Texture/TextureManager.h"

using namespace DX12Engine::Renderer;
using namespace DX12Engine::Resource;
using namespace DX12Engine::ECS;

namespace DX12Engine::Renderer {

// 分组键：相同 Mesh + 相同 Material 的实体可以合批
struct BatchKey {
    GeometryHandle geometry;
    uint32_t materialIdx;

    bool operator==(const BatchKey &other) const {
        return geometry.index == other.geometry.index && materialIdx == other.materialIdx;
    }
};

struct BatchKeyHash {
    size_t operator()(const BatchKey &key) const { return ((size_t)key.geometry.index << 32) ^ (key.materialIdx << 1); }
};

OpaqueRenderItemBuilder::OpaqueRenderItemBuilder(FrameResourceManager *frameResources, MaterialManager *materialManager,
                                                 TextureManager *textureManager)
    : m_frameResourceManager(frameResources), m_materialManager(materialManager), m_textureManager(textureManager) {}

void OpaqueRenderItemBuilder::BuildTyped(ECS::Registry &registry, TRenderQueue<OpaqueRenderItem> &outQueue) {
    outQueue.Clear();

    struct BatchEntry {
        std::vector<InstanceData> instances;
        std::vector<Entity> entities;
        Resource::TextureHandle textureHandle;
        uint32_t probeIndex = UINT32_MAX; // 批次探针索引（批次内所有实体应一致）
    };
    std::unordered_map<BatchKey, BatchEntry, BatchKeyHash> batches;

    for (auto entity : m_cullingResult->visibleEntities) {
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

        InstanceData instData = {};
        XMMATRIX world = transform->GetMatrix();
        XMMATRIX worldInvTranspose = XMMatrixTranspose(XMMatrixInverse(nullptr, world));
        XMStoreFloat4x4(&instData.World, world);
        XMStoreFloat4x4(&instData.WorldInvTranspose, worldInvTranspose);
        instData.MaterialIndex = materialIdx;
        instData.ReceiveShadow = meshComp->receivesShadow ? 1 : 0;

        // 读取 ReflectionConsumerComponent，确定探针索引
        uint32_t probeIdx = UINT32_MAX;
        auto *reflectionComp = registry.TryGetComponent<ReflectionConsumerComponent>(entity);
        if (reflectionComp) {
            probeIdx = reflectionComp->probeIndex;
            // TODO: 当 probeIndex == UINT32_MAX && useDynamicFallback == true 时，
            //       调用 ReflectionProbeManager::FindClosestProbe() 做动态回退
        }
        instData.ProbeIndex = probeIdx;

        BatchKey key{geoHandle, materialIdx};
        auto &entry = batches[key];
        entry.instances.push_back(instData);
        entry.textureHandle = meshComp->textureHandle;
        entry.probeIndex = probeIdx; // 批次内所有物体共享同一探针索引
    }

    for (auto &[key, entry] : batches) {
        D3D12_GPU_DESCRIPTOR_HANDLE textureSRV = m_textureManager->GetSRV(entry.textureHandle);
        auto &instances = entry.instances;

        D3D12_GPU_VIRTUAL_ADDRESS instanceBuffer = m_frameResourceManager->AllocateInstance(
            instances.data(), static_cast<uint32_t>(instances.size() * sizeof(InstanceData)));

        OpaqueRenderItem item = OpaqueRenderItem::Create(key.geometry, key.materialIdx, textureSRV, instanceBuffer,
                                                         (uint32_t)instances.size(), entry.probeIndex);
        outQueue.Add(item);
    }
}

} // namespace DX12Engine::Renderer
