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
    // 单队列兼容模式：按原有逻辑构建
    outQueue.Clear();

    // 扩展 batch：同时记录 TextureHandle 用于获取 SRV 及实体列表
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
            // 静态物体：直接复用缓存的矩阵，跳过 XMMatrixInverse
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

    constexpr uint32_t MIN_INSTANCE_COUNT = 2;

    for (auto &[key, entry] : batches) {
        D3D12_GPU_DESCRIPTOR_HANDLE textureSRV = m_textureManager->GetSRV(entry.textureHandle);
        auto &instances = entry.instances;

        if (instances.size() >= MIN_INSTANCE_COUNT) {
            // 实例化模式
            D3D12_GPU_VIRTUAL_ADDRESS instanceBuffer;

            if (key.isStatic) {
                Entity firstEntity = entry.entities[0];
                auto *staticComp = registry.TryGetComponent<StaticComponent>(firstEntity);

                if (staticComp->persistentInstanceAddress == 0) {
                    uint32_t dataSize = static_cast<uint32_t>(instances.size() * sizeof(InstanceData));
                    instanceBuffer =
                        m_frameResourceManager->AllocatePersistentInstanceBuffer(instances.data(), dataSize);
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
                instanceBuffer =
                    m_frameResourceManager->AllocateInstance(instances.data(), instances.size() * sizeof(InstanceData));
            }

            OpaqueRenderItem item = OpaqueRenderItem::CreateInstanced(key.geometry, key.materialIdx, textureSRV,
                                                                      instanceBuffer, (uint32_t)instances.size());
            outQueue.Add(item);
        } else {
            // 单物体模式
            for (size_t i = 0; i < instances.size(); ++i) {
                auto &instData = instances[i];
                Entity entity = entry.entities[i];

                ObjectConstants objCB;
                objCB.World = instData.World;
                objCB.WorldInvTranspose = instData.WorldInvTranspose;
                objCB.MaterialIndex = instData.MaterialIndex;
                objCB.ReceiveShadow = instData.ReceiveShadow;

                D3D12_GPU_VIRTUAL_ADDRESS cbAddress;

                if (key.isStatic) {
                    auto *staticComp = registry.TryGetComponent<StaticComponent>(entity);

                    if (staticComp->persistentCBAddress == 0) {
                        cbAddress = m_frameResourceManager->AllocatePersistentObjectCB(&objCB, sizeof(ObjectConstants));
                        staticComp->persistentCBAddress = cbAddress;
                        staticComp->worldDirty = false;
                    } else {

                        // 如果静态对象的世界矩阵有变化，更新持久化缓冲区
                        // 存在线程安全性问题，无法保障多线程环境下的数据一致性
                        if (staticComp->worldDirty) {
                            m_frameResourceManager->UpdatePersistentBuffer(staticComp->persistentCBAddress, &objCB,
                                                                           sizeof(ObjectConstants));
                            staticComp->worldDirty = false;
                        }
                        cbAddress = staticComp->persistentCBAddress;
                    }
                } else {
                    cbAddress = m_frameResourceManager->AllocateObjectCB(&objCB, sizeof(ObjectConstants));
                }

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

    // 1. 收集实例数据，按 (Geometry, Material, isStatic) 分组
    struct BatchEntry {
        std::vector<InstanceData> instances;
        std::vector<Entity> entities; // 记录实体，用于读取 StaticComponent
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
            // 静态物体：直接复用缓存的矩阵，跳过 XMMatrixInverse
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

    // 2. 为每个批次生成 RenderItem，静态/动态分离处理
    constexpr uint32_t MIN_INSTANCE_COUNT = 2;

    for (auto &[key, entry] : batches) {
        D3D12_GPU_DESCRIPTOR_HANDLE textureSRV = m_textureManager->GetSRV(entry.textureHandle);

        if (textureSRV.ptr == 0 && entry.textureHandle.IsValid()) {
            char msg[256];
            sprintf_s(msg, "[WARN] OpaqueRenderItemBuilder: GetSRV returned null for textureHandle index=%u gen=%u\n",
                      entry.textureHandle.index, entry.textureHandle.generation);
            OutputDebugStringA(msg);
        }

        auto &instances = entry.instances;

        if (instances.size() >= MIN_INSTANCE_COUNT) {
            // ================================================================
            // 实例化模式
            // ================================================================
            D3D12_GPU_VIRTUAL_ADDRESS instanceBuffer;

            if (key.isStatic) {
                // 静态批次：使用持久化实例缓冲区
                Entity firstEntity = entry.entities[0];
                auto *staticComp = registry.TryGetComponent<StaticComponent>(firstEntity);

                if (staticComp->persistentInstanceAddress == 0) {
                    // 首次分配
                    uint32_t dataSize = static_cast<uint32_t>(instances.size() * sizeof(InstanceData));
                    instanceBuffer =
                        m_frameResourceManager->AllocatePersistentInstanceBuffer(instances.data(), dataSize);
                    staticComp->persistentInstanceAddress = instanceBuffer;
                    staticComp->worldDirty = false;
                } else {
                    // 检查脏标记，仅更新变化的数据
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
                // 动态批次：每帧使用环形缓冲区
                instanceBuffer =
                    m_frameResourceManager->AllocateInstance(instances.data(), instances.size() * sizeof(InstanceData));
            }

            OpaqueRenderItem item = OpaqueRenderItem::CreateInstanced(key.geometry, key.materialIdx, textureSRV,
                                                                      instanceBuffer, (uint32_t)instances.size());
            outInstanced.Add(item);
        } else {
            // ================================================================
            // 单物体模式
            // ================================================================
            for (size_t i = 0; i < instances.size(); ++i) {
                auto &instData = instances[i];
                Entity entity = entry.entities[i];

                ObjectConstants objCB;
                objCB.World = instData.World;
                objCB.WorldInvTranspose = instData.WorldInvTranspose;
                objCB.MaterialIndex = instData.MaterialIndex;
                objCB.ReceiveShadow = instData.ReceiveShadow;

                D3D12_GPU_VIRTUAL_ADDRESS cbAddress;

                if (key.isStatic) {
                    auto *staticComp = registry.TryGetComponent<StaticComponent>(entity);

                    if (staticComp->persistentCBAddress == 0) {
                        // 首次分配持久化 CB
                        cbAddress = m_frameResourceManager->AllocatePersistentObjectCB(&objCB, sizeof(ObjectConstants));
                        staticComp->persistentCBAddress = cbAddress;
                        staticComp->worldDirty = false;
                    } else {
                        // 脏标记：仅更新变化的数据
                        // 存在线程安全性问题，无法保障多线程环境下的数据一致性
                        if (staticComp->worldDirty) {
                            m_frameResourceManager->UpdatePersistentBuffer(staticComp->persistentCBAddress, &objCB,
                                                                           sizeof(ObjectConstants));
                            staticComp->worldDirty = false;
                        }
                        cbAddress = staticComp->persistentCBAddress;
                    }
                } else {
                    // 动态物体：每帧使用环形缓冲区
                    cbAddress = m_frameResourceManager->AllocateObjectCB(&objCB, sizeof(ObjectConstants));
                }

                OpaqueRenderItem item =
                    OpaqueRenderItem::CreateStandard(key.geometry, key.materialIdx, textureSRV, cbAddress);
                outStandard.Add(item);
            }
        }
    }
}

} // namespace DX12Engine::Renderer