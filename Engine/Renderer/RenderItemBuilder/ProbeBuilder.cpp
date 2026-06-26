#include "ProbeBuilder.h"
#include "ECS/Core/Components.h"
#include "Math/BoundingVolume.h"
#include "Renderer/FrameResources/FrameResourceManager.h"
#include "Resource/Manager/MaterialManager.h"
#include "Resource/Texture/TextureManager.h"

using namespace DX12Engine::ECS;
using namespace DX12Engine::Resource;

namespace DX12Engine::Renderer {

struct ProbeBatchKey {
    Resource::GeometryHandle geometry;
    uint32_t materialIdx;

    bool operator==(const ProbeBatchKey &other) const {
        return geometry.index == other.geometry.index && materialIdx == other.materialIdx;
    }
};

struct ProbeBatchKeyHash {
    size_t operator()(const ProbeBatchKey &key) const {
        return ((size_t)key.geometry.index << 32) ^ (key.materialIdx << 1);
    }
};

ProbeBuilder::ProbeBuilder(FrameResourceManager *frameResources, Resource::MaterialManager *materialManager,
                           Resource::TextureManager *textureManager)
    : m_frameResourceManager(frameResources), m_materialManager(materialManager), m_textureManager(textureManager) {}

/**
 * @brief  构建反射探针渲染项
 * @param probes 反射探针信息数组
 * @param probeCount 反射探针数量
 * @param registry ECS注册表
 * @param outputQueues 输出渲染队列数组，每个元素对应一个反射探针的渲染队列
 * @date 2026-06-26
 */
void ProbeBuilder::Build(const ProbeCaptureInfo *probes, uint32_t probeCount, ECS::Registry &registry,
                         TRenderQueue<OpaqueRenderItem> *outputQueues) {
    if (!probes || probeCount == 0)
        return;

    for (uint32_t p = 0; p < probeCount; ++p) {
        outputQueues[p].Clear();
    }

    auto view = registry.view<MeshComponent, TransformComponent>();

    for (uint32_t p = 0; p < probeCount; ++p) {
        const auto &probe = probes[p];
        auto &outQueue = outputQueues[p];

        DirectX::XMVECTOR probePos = DirectX::XMLoadFloat3(&probe.position);

        struct ProbeBatchEntry {
            std::vector<InstanceData> instances;
            Resource::TextureHandle textureHandle;
        };
        std::unordered_map<ProbeBatchKey, ProbeBatchEntry, ProbeBatchKeyHash> batches;

        for (auto entity : view) {
            auto *meshComp = registry.TryGetComponent<MeshComponent>(entity);
            if (!meshComp)
                continue;

            GeometryHandle geoHandle;
            if (m_lodSystem && meshComp->lodMeshHandle.IsValid()) {
                const auto *lodMesh = m_lodSystem->GetLODMesh(meshComp->lodMeshHandle);
                geoHandle = lodMesh ? lodMesh->GetHighestLOD() : GeometryHandle::Invalid();
            }
            if (!geoHandle.IsValid())
                continue;

            auto *transform = registry.TryGetComponent<TransformComponent>(entity);
            if (!transform)
                continue;

            // 世界空间包围球 + captureRange 剔除
            Math::BoundingSphere localSphere;
            std::visit(
                [&](const auto &bounds) {
                    using T = std::decay_t<decltype(bounds)>;
                    if constexpr (std::is_same_v<T, Math::BoundingSphere>) {
                        localSphere = bounds; // 直接拷贝
                    } else {
                        localSphere = bounds.ToSphere(); // 转换
                    }
                },
                meshComp->localBounds);
            DirectX::XMMATRIX world = transform->GetMatrix();
            DirectX::XMVECTOR worldCenter =
                DirectX::XMVector3Transform(DirectX::XMLoadFloat3(&localSphere.center), world);
            float scaleX = XMVectorGetX(XMVector3Length(world.r[0]));
            float scaleY = XMVectorGetX(XMVector3Length(world.r[1]));
            float scaleZ = XMVectorGetX(XMVector3Length(world.r[2]));
            float worldRadius = localSphere.radius * std::max({scaleX, scaleY, scaleZ});

            DirectX::XMVECTOR delta = DirectX::XMVectorSubtract(worldCenter, probePos);
            float distSq = XMVectorGetX(DirectX::XMVector3LengthSq(delta));
            float combined = probe.captureRange + worldRadius;
            if (distSq > combined * combined)
                continue;

            uint32_t materialIdx = m_materialManager->GetGPUIndex(meshComp->materialHandle);

            InstanceData instData = {};
            DirectX::XMMATRIX worldInvTranspose = DirectX::XMMatrixTranspose(DirectX::XMMatrixInverse(nullptr, world));
            DirectX::XMStoreFloat4x4(&instData.World, world);
            DirectX::XMStoreFloat4x4(&instData.WorldInvTranspose, worldInvTranspose);
            instData.MaterialIndex = materialIdx;
            instData.ReceiveShadow = meshComp->receivesShadow ? 1 : 0;
            instData.ProbeIndex = probe.probeIndex;

            ProbeBatchKey key{geoHandle, materialIdx};
            batches[key].instances.push_back(instData);
            batches[key].textureHandle = meshComp->textureHandle;
        }

        for (auto &[key, entry] : batches) {
            D3D12_GPU_DESCRIPTOR_HANDLE textureSRV = m_textureManager->GetSRV(entry.textureHandle);
            auto &instances = entry.instances;

            D3D12_GPU_VIRTUAL_ADDRESS instanceBuffer = m_frameResourceManager->AllocateInstance(
                instances.data(), static_cast<uint32_t>(instances.size() * sizeof(InstanceData)));

            OpaqueRenderItem item = OpaqueRenderItem::Create(key.geometry, key.materialIdx, textureSRV, instanceBuffer,
                                                             (uint32_t)instances.size(), probe.probeIndex);
            outQueue.Add(item);
        }
    }
}

} // namespace DX12Engine::Renderer
