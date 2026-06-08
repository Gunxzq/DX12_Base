#include "BillboardRenderItemBuilder.h"
#include "ECS/Core/Components.h"
#include "Renderer/FrameResources/FrameResourceManager.h"
#include "Resource/Manager/MaterialManager.h"
#include "Resource/Texture/TextureManager.h"
#include <vector>

using namespace DX12Engine::Renderer;
using namespace DX12Engine::Resource;
using namespace DX12Engine::ECS;

namespace DX12Engine::Renderer {

// 公告牌实例数据（与着色器中的布局一致）
struct BillboardInstanceData {
    DirectX::XMFLOAT3 Position;
    float Width;
    float Height;
    uint32_t Mode;
    uint32_t TextureArrayIndex;
    uint32_t MaterialIndex;
};

BillboardRenderItemBuilder::BillboardRenderItemBuilder(FrameResourceManager *frameResources,
                                                       TextureManager *textureManager, MaterialManager *materialManager)
    : m_frameResourceManager(frameResources), m_textureManager(textureManager), m_materialManager(materialManager) {}

void BillboardRenderItemBuilder::BuildTyped(ECS::Registry &registry, TRenderQueue<BillboardRenderItem> &outQueue) {
    outQueue.Clear();

    if (!m_cullingResult || !m_lodResult) {
        return;
    }

    // 所有公告牌共享同一个 Texture2DArray，通过实例数据中的 TextureArrayIndex 区分切片
    // 按 isStatic 分两组
    struct BatchEntry {
        std::vector<BillboardInstanceData> instances;
        Entity firstEntity = INVALID_ENTITY; // 用于静态批次的持久化地址
    };
    BatchEntry staticBatch;
    BatchEntry dynamicBatch;

    for (auto entity : m_cullingResult->visibleEntities) {
        auto *billboardComp = registry.TryGetComponent<BillboardComponent>(entity);
        if (!billboardComp)
            continue;

        auto *transform = registry.TryGetComponent<TransformComponent>(entity);
        if (!transform)
            continue;

        // 距离剔除
        float dx = transform->position.x - m_cameraPos.x;
        float dy = transform->position.y - m_cameraPos.y;
        float dz = transform->position.z - m_cameraPos.z;
        float distSq = dx * dx + dy * dy + dz * dz;

        float minDist = billboardComp->minDistance;
        float maxDist = billboardComp->maxDistance;
        if (distSq < minDist * minDist || distSq > maxDist * maxDist) {
            continue;
        }

        // 验证纹理句柄
        if (!m_textureManager->IsValid(billboardComp->textureHandle))
            continue;

        // 构建实例数据
        BillboardInstanceData inst;
        inst.Position = transform->position;
        inst.Width = billboardComp->width;
        inst.Height = billboardComp->height;
        inst.Mode = static_cast<uint32_t>(billboardComp->mode);
        inst.TextureArrayIndex = billboardComp->textureArrayIndex; // Texture2DArray 切片索引
        inst.MaterialIndex = m_materialManager->GetGPUIndex(billboardComp->materialHandle);

        auto *staticComp = registry.TryGetComponent<StaticComponent>(entity);
        if (staticComp) {
            if (staticBatch.firstEntity == INVALID_ENTITY)
                staticBatch.firstEntity = entity;
            staticBatch.instances.push_back(inst);
        } else {
            dynamicBatch.instances.push_back(inst);
        }
    }

    // 处理静态公告牌批次
    if (!staticBatch.instances.empty()) {
        D3D12_GPU_VIRTUAL_ADDRESS instanceBufferAddress;
        auto *staticComp = registry.TryGetComponent<StaticComponent>(staticBatch.firstEntity);

        if (staticComp->persistentInstanceAddress == 0) {
            // 首次分配持久化实例缓冲区
            uint32_t dataSize = static_cast<uint32_t>(staticBatch.instances.size() * sizeof(BillboardInstanceData));
            instanceBufferAddress =
                m_frameResourceManager->AllocatePersistentInstanceBuffer(staticBatch.instances.data(), dataSize);
            staticComp->persistentInstanceAddress = instanceBufferAddress;
            staticComp->worldDirty = false;
        } else {
            if (staticComp->worldDirty) {
                uint32_t dataSize = static_cast<uint32_t>(staticBatch.instances.size() * sizeof(BillboardInstanceData));
                m_frameResourceManager->UpdatePersistentBuffer(staticComp->persistentInstanceAddress,
                                                               staticBatch.instances.data(), dataSize);
                staticComp->worldDirty = false;
            }
            instanceBufferAddress = staticComp->persistentInstanceAddress;
        }

        if (instanceBufferAddress != 0) {
            BillboardRenderItem item;
            item.instanceBufferAddress = instanceBufferAddress;
            item.instanceCount = static_cast<uint32_t>(staticBatch.instances.size());
            outQueue.Add(item);
        }
    }

    // 处理动态公告牌批次
    if (!dynamicBatch.instances.empty()) {
        D3D12_GPU_VIRTUAL_ADDRESS instanceBufferAddress = m_frameResourceManager->AllocateInstance(
            dynamicBatch.instances.data(),
            static_cast<uint32_t>(dynamicBatch.instances.size() * sizeof(BillboardInstanceData)));

        if (instanceBufferAddress != 0) {
            BillboardRenderItem item;
            item.instanceBufferAddress = instanceBufferAddress;
            item.instanceCount = static_cast<uint32_t>(dynamicBatch.instances.size());
            outQueue.Add(item);
        }
    }
}

} // namespace DX12Engine::Renderer