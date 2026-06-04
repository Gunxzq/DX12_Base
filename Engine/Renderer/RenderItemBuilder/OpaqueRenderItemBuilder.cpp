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
    size_t operator()(const BatchKey &key) const { return ((size_t)key.geometry.index << 32) ^ key.materialIdx; }
};

OpaqueRenderItemBuilder::OpaqueRenderItemBuilder(FrameResourceManager *frameResources, MaterialManager *materialManager,
                                                 TextureManager *textureManager)
    : m_frameResourceManager(frameResources), m_materialManager(materialManager), m_textureManager(textureManager) {}

void OpaqueRenderItemBuilder::BuildTyped(ECS::Registry &registry, TRenderQueue<OpaqueRenderItem> &outQueue) {
    // 单队列兼容模式：按原有逻辑构建
    outQueue.Clear();

    // 扩展 batch：同时记录 TextureHandle 用于获取 SRV
    struct BatchEntry {
        std::vector<InstanceData> instances;
        Resource::TextureHandle textureHandle;
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

        InstanceData instData;
        XMMATRIX world = transform->GetMatrix();
        XMMATRIX worldInvTranspose = XMMatrixTranspose(XMMatrixInverse(nullptr, world));

        XMStoreFloat4x4(&instData.World, world);
        XMStoreFloat4x4(&instData.WorldInvTranspose, worldInvTranspose);
        instData.MaterialIndex = materialIdx;
        instData.ReceiveShadow = meshComp->receivesShadow ? 1 : 0;

        BatchKey key{geoHandle, materialIdx};
        auto &entry = batches[key];
        entry.instances.push_back(instData);
        // 同一批次内纹理相同，取第一个即可
        if (!entry.textureHandle.IsValid()) {
            entry.textureHandle = meshComp->textureHandle;
        }
    }

    constexpr uint32_t MIN_INSTANCE_COUNT = 2;

    for (auto &[key, entry] : batches) {
        // 从 TextureManager 获取实际的 GPU SRV handle
        D3D12_GPU_DESCRIPTOR_HANDLE textureSRV = m_textureManager->GetSRV(entry.textureHandle);
        auto &instances = entry.instances;

        if (instances.size() >= MIN_INSTANCE_COUNT) {
            D3D12_GPU_VIRTUAL_ADDRESS instanceBuffer =
                m_frameResourceManager->AllocateInstance(instances.data(), instances.size() * sizeof(InstanceData));

            OpaqueRenderItem item = OpaqueRenderItem::CreateInstanced(key.geometry, key.materialIdx, textureSRV,
                                                                      instanceBuffer, (uint32_t)instances.size());
            outQueue.Add(item);
        } else {
            for (auto &instData : instances) {
                ObjectConstants objCB;
                objCB.World = instData.World;
                objCB.WorldInvTranspose = instData.WorldInvTranspose;
                objCB.MaterialIndex = instData.MaterialIndex;
                objCB.ReceiveShadow = instData.ReceiveShadow;

                D3D12_GPU_VIRTUAL_ADDRESS cbAddress =
                    m_frameResourceManager->AllocateObjectCB(&objCB, sizeof(ObjectConstants));

                OpaqueRenderItem item =
                    OpaqueRenderItem::CreateStandard(key.geometry, key.materialIdx, textureSRV, cbAddress);
                outQueue.Add(item);
            }
        }
    }
}

void OpaqueRenderItemBuilder::BuildDualQueue(ECS::Registry &registry, TRenderQueue<OpaqueRenderItem> &outStandard,
                                             TRenderQueue<OpaqueRenderItem> &outInstanced) {
    outStandard.Clear();
    outInstanced.Clear();

    // 1. 收集实例数据，按 (Geometry, Material) 分组
    struct BatchEntry {
        std::vector<InstanceData> instances;
        Resource::TextureHandle textureHandle;
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

        InstanceData instData;
        XMMATRIX world = transform->GetMatrix();
        XMMATRIX worldInvTranspose = XMMatrixTranspose(XMMatrixInverse(nullptr, world));

        XMStoreFloat4x4(&instData.World, world);
        XMStoreFloat4x4(&instData.WorldInvTranspose, worldInvTranspose);
        instData.MaterialIndex = materialIdx;
        instData.ReceiveShadow = meshComp->receivesShadow ? 1 : 0;

        BatchKey key{geoHandle, materialIdx};
        auto &entry = batches[key];
        entry.instances.push_back(instData);
        if (!entry.textureHandle.IsValid()) {
            entry.textureHandle = meshComp->textureHandle;
        }
    }

    // 2. 为每个批次生成 RenderItem，按类型分入不同队列
    constexpr uint32_t MIN_INSTANCE_COUNT = 2;

    for (auto &[key, entry] : batches) {
        D3D12_GPU_DESCRIPTOR_HANDLE textureSRV = m_textureManager->GetSRV(entry.textureHandle);

        // 诊断：验证纹理 SRV 是否有效
        if (textureSRV.ptr == 0 && entry.textureHandle.IsValid()) {
            char msg[256];
            sprintf_s(msg, "[WARN] OpaqueRenderItemBuilder: GetSRV returned null for textureHandle index=%u gen=%u\n",
                      entry.textureHandle.index, entry.textureHandle.generation);
            OutputDebugStringA(msg);
        }

        auto &instances = entry.instances;

        if (instances.size() >= MIN_INSTANCE_COUNT) {
            // 实例化模式 → Instanced 队列
            D3D12_GPU_VIRTUAL_ADDRESS instanceBuffer =
                m_frameResourceManager->AllocateInstance(instances.data(), instances.size() * sizeof(InstanceData));

            OpaqueRenderItem item = OpaqueRenderItem::CreateInstanced(key.geometry, key.materialIdx, textureSRV,
                                                                      instanceBuffer, (uint32_t)instances.size());
            outInstanced.Add(item);
        } else {
            // 单物体模式 → Standard 队列
            for (auto &instData : instances) {
                ObjectConstants objCB;
                objCB.World = instData.World;
                objCB.WorldInvTranspose = instData.WorldInvTranspose;
                objCB.MaterialIndex = instData.MaterialIndex;
                objCB.ReceiveShadow = instData.ReceiveShadow;

                D3D12_GPU_VIRTUAL_ADDRESS cbAddress =
                    m_frameResourceManager->AllocateObjectCB(&objCB, sizeof(ObjectConstants));

                OpaqueRenderItem item =
                    OpaqueRenderItem::CreateStandard(key.geometry, key.materialIdx, textureSRV, cbAddress);
                outStandard.Add(item);
            }
        }
    }
}

} // namespace DX12Engine::Renderer