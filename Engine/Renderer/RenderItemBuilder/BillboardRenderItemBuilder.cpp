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

    // TODO(StaticComponent): 静态优化暂未启用，全部走动态路径
    //  所有公告牌每帧重新分配实例缓冲区
    struct BatchEntry {
        std::vector<BillboardInstanceData> instances;
    };
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

        // TODO(StaticComponent): 静态优化暂未启用，全部走动态路径
        dynamicBatch.instances.push_back(inst);
    }

    // TODO(StaticComponent): 静态批次处理已禁用，所有公告牌走动态路径
    // 处理动态公告牌批次（即全部公告牌）
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