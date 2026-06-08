#include "OpaqueRenderItemBuilder.h"
#include "ECS/Core/Components.h"
#include "Renderer/FrameResources/FrameResourceManager.h"
#include "Resource/Manager/MaterialManager.h"
#include "Resource/Texture/TextureManager.h"

using namespace DX12Engine::Renderer;
using namespace DX12Engine::Resource;
using namespace DX12Engine::ECS;

namespace DX12Engine::Renderer {

// 分组键：相同 Mesh + 相同 Material + 相同静态/动态标志的实体可以合批
struct BatchKey {
    GeometryHandle geometry;
    uint32_t materialIdx;
    bool isStatic;

    bool operator==(const BatchKey &other) const {
        return geometry.index == other.geometry.index && materialIdx == other.materialIdx && isStatic == other.isStatic;
    }
};

struct BatchKeyHash {
    size_t operator()(const BatchKey &key) const {
        return ((size_t)key.geometry.index << 32) ^ (key.materialIdx << 1) ^ (key.isStatic ? 1 : 0);
    }
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

        auto *staticComp = registry.TryGetComponent<StaticComponent>(entity);
        bool isStatic = (staticComp != nullptr);

        uint32_t materialIdx = m_materialManager->GetGPUIndex(meshComp->materialHandle);

        InstanceData instData;

        if (isStatic && !staticComp->worldDirty) {
            instData.World = staticComp->cachedWorld;
            instData.WorldInvTranspose = staticComp->cachedWorldInvTranspose;
        } else {
            XMMATRIX world = transform->GetMatrix();
            XMMATRIX worldInvTranspose = XMMatrixTranspose(XMMatrixInverse(nullptr, world));
            XMStoreFloat4x4(&instData.World, world);
            XMStoreFloat4x4(&instData.WorldInvTranspose, worldInvTranspose);

            // 静态物体首次计算：缓存结果
            // 存在线程安全性问题，无法保障多线程环境下的数据一致性
            if (isStatic) {
                staticComp->cachedWorld = instData.World;
                staticComp->cachedWorldInvTranspose = instData.WorldInvTranspose;
            }
        }

        instData.MaterialIndex = materialIdx;
        instData.ReceiveShadow = meshComp->receivesShadow ? 1 : 0;

        BatchKey key{geoHandle, materialIdx, isStatic};
        auto &entry = batches[key];
        entry.instances.push_back(instData);
        entry.entities.push_back(entity);
        if (!entry.textureHandle.IsValid()) {
            entry.textureHandle = meshComp->textureHandle;
        }
    }

    for (auto &[key, entry] : batches) {
        D3D12_GPU_DESCRIPTOR_HANDLE textureSRV = m_textureManager->GetSRV(entry.textureHandle);
        auto &instances = entry.instances;

        // 统一实例化模式：所有批次都走 Instanced 路径（单物体 instanceCount=1）
        D3D12_GPU_VIRTUAL_ADDRESS instanceBuffer;

        if (key.isStatic) {
            Entity firstEntity = entry.entities[0];
            auto *staticComp = registry.TryGetComponent<StaticComponent>(firstEntity);

            if (staticComp->persistentInstanceAddress == 0) {
                uint32_t dataSize = static_cast<uint32_t>(instances.size() * sizeof(InstanceData));
                instanceBuffer = m_frameResourceManager->AllocatePersistentInstanceBuffer(instances.data(), dataSize);
                staticComp->persistentInstanceAddress = instanceBuffer;
                staticComp->worldDirty = false;
            } else {
                // 静态物体：检查是否有世界矩阵更新
                // 存在线程安全性问题，无法保障多线程环境下的数据一致性
                if (staticComp->worldDirty) {
                    uint32_t dataSize = static_cast<uint32_t>(instances.size() * sizeof(InstanceData));
                    m_frameResourceManager->UpdatePersistentBuffer(staticComp->persistentInstanceAddress,
                                                                   instances.data(), dataSize);
                    staticComp->worldDirty = false;
                }
                instanceBuffer = staticComp->persistentInstanceAddress;
            }
        } else {
            instanceBuffer = m_frameResourceManager->AllocateInstance(
                instances.data(), static_cast<uint32_t>(instances.size() * sizeof(InstanceData)));
        }

        OpaqueRenderItem item = OpaqueRenderItem::Create(key.geometry, key.materialIdx, textureSRV, instanceBuffer,
                                                         (uint32_t)instances.size());
        outQueue.Add(item);
    }
}

} // namespace DX12Engine::Renderer
