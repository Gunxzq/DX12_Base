#include "BillboardRenderItemBuilder.h"
#include "ECS/Core/Components.h"
#include "Renderer/FrameResources/FrameResourceManager.h"
#include "Resource/Texture/TextureManager.h"
#include <unordered_map>
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
    float Pad;
};

// 分组键：相同纹理的公告牌可以合批
struct BatchKey {
    D3D12_GPU_DESCRIPTOR_HANDLE textureSRV;

    bool operator==(const BatchKey &other) const { return textureSRV.ptr == other.textureSRV.ptr; }
};

struct BatchKeyHash {
    size_t operator()(const BatchKey &key) const { return std::hash<uint64_t>()(key.textureSRV.ptr); }
};

BillboardRenderItemBuilder::BillboardRenderItemBuilder(FrameResourceManager *frameResources,
                                                       TextureManager *textureManager)
    : m_frameResourceManager(frameResources), m_textureManager(textureManager) {}

void BillboardRenderItemBuilder::BuildTyped(ECS::Registry &registry, TRenderQueue<BillboardRenderItem> &outQueue) {
    outQueue.Clear();

    if (!m_cullingResult || !m_lodResult) {
        return;
    }

    // 按纹理分组
    std::unordered_map<BatchKey, std::vector<BillboardInstanceData>, BatchKeyHash> batches;

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

        // 获取纹理 SRV
        D3D12_GPU_DESCRIPTOR_HANDLE textureSRV = m_textureManager->GetSRV(billboardComp->textureHandle);
        if (textureSRV.ptr == 0)
            continue;

        // 获取 SRV 绝对索引（用于无界纹理数组 gSharedTextures[TexIndex]）
        uint32_t srvIndex = m_textureManager->GetSRVIndex(billboardComp->textureHandle);
        if (srvIndex == UINT32_MAX)
            continue;

        // 构建实例数据
        BillboardInstanceData inst;
        inst.Position = transform->position;
        inst.Width = billboardComp->width;
        inst.Height = billboardComp->height;
        inst.Mode = static_cast<uint32_t>(billboardComp->mode);
        inst.TextureArrayIndex = srvIndex; // 绝对 SRV 索引，供无界纹理数组使用

        BatchKey key{textureSRV};
        batches[key].push_back(inst);
    }

    // 为每个批次生成渲染项
    for (auto &[key, instances] : batches) {
        if (instances.empty())
            continue;

        // 上传实例数据到 GPU
        D3D12_GPU_VIRTUAL_ADDRESS instanceBufferAddress = m_frameResourceManager->AllocateInstance(
            instances.data(), static_cast<uint32_t>(instances.size() * sizeof(BillboardInstanceData)));

        if (instanceBufferAddress == 0)
            continue;

        BillboardRenderItem item;
        item.instanceBufferAddress = instanceBufferAddress;
        item.instanceCount = static_cast<uint32_t>(instances.size());
        item.textureSRV = key.textureSRV;

        outQueue.Add(item);
    }
}

} // namespace DX12Engine::Renderer