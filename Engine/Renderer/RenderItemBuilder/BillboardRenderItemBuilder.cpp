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

    struct BatchEntry {
        std::vector<BillboardInstanceData> instances;
    };
    BatchEntry dynamicBatch;

    auto view = registry.view<BillboardComponent, TransformComponent>();
    for (auto entity : view) {
        auto &billboardComp = view.get<BillboardComponent>(entity);
        auto &transform = view.get<TransformComponent>(entity);

        // 距离剔除（公告牌自身有 min/maxDistance）
        float dx = transform.position.x - m_cameraPos.x;
        float dy = transform.position.y - m_cameraPos.y;
        float dz = transform.position.z - m_cameraPos.z;
        float distSq = dx * dx + dy * dy + dz * dz;
        float minDist = billboardComp.minDistance;
        float maxDist = billboardComp.maxDistance;
        if (distSq < minDist * minDist || distSq > maxDist * maxDist)
            continue;

        // 验证纹理
        if (!m_textureManager->IsValid(billboardComp.textureHandle))
            continue;

        BillboardInstanceData inst;
        inst.Position = transform.position;
        inst.Width = billboardComp.width;
        inst.Height = billboardComp.height;
        inst.Mode = static_cast<uint32_t>(billboardComp.mode);
        inst.TextureArrayIndex = billboardComp.textureArrayIndex;
        inst.MaterialIndex = m_materialManager->GetGPUIndex(billboardComp.materialHandle);
        dynamicBatch.instances.push_back(inst);
    }

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
