#include "RenderItemBuilder.h"

#include "Common/Common.h"

#include "ECS/Core/Components.h"
#include "Renderer/Scene/CameraManager.h"
#include "Resource/Manager/MaterialManager.h"

using namespace DX12Engine::Renderer;
using namespace DX12Engine::Resource;

namespace DX12Engine::Renderer {

// ============================================================================
// 执行
// ============================================================================

void RenderItemBuilder::Execute(ECS::Registry &registry, const CullingResult &cullingResult, const LODResult &lodResult,
                                RenderQueue &outOpaqueQueue, RenderQueue &outTransparentQueue) {

    outOpaqueQueue.Clear();
    outTransparentQueue.Clear();

    DirectX::XMFLOAT3 cameraPos = {0.0f, 0.0f, 0.0f};
    if (m_cameraManager) {
        const auto &camera = m_cameraManager->GetMainCamera();
        cameraPos = camera.Position;
    }

    for (auto entity : cullingResult.visibleEntities) {
        // 尝试不透明组件
        auto *opaqueComp = registry.TryGetComponent<ECS::MeshComponent>(entity);
        if (opaqueComp) {
            BuildRenderItem(registry, entity, *opaqueComp, lodResult, cameraPos, outOpaqueQueue, false);
            continue;
        }

        // 尝试透明组件
        auto *transparentComp = registry.TryGetComponent<ECS::TransparentMeshComponent>(entity);
        if (transparentComp) {
            BuildRenderItem(registry, entity, *transparentComp, lodResult, cameraPos, outTransparentQueue, true);
            continue;
        }
    }

    // 分别排序（透明物体需要远到近）
    outOpaqueQueue.Sort(); // 不透明物体按 sortKey 升序（近到远）
    // 透明物体排序在 BuildSortKey 中已处理（depth 取反）
    outTransparentQueue.Sort();
}

// ============================================================================
// 模板辅助函数
// ============================================================================

template <typename MeshCompType>
void RenderItemBuilder::BuildRenderItem(ECS::Registry &registry, ECS::Entity entity, MeshCompType &meshComp,
                                        const LODResult &lodResult, const DirectX::XMFLOAT3 &cameraPos,
                                        RenderQueue &outQueue, bool isTransparent) {

    Resource::GeometryHandle handle = lodResult.GetHandle(entity);
    if (!handle.IsValid())
        return;

    Resource::MaterialHandle materialHandle = meshComp.materialHandle;
    Resource::TextureHandle textureHandle = meshComp.textureHandle;

    if (!materialHandle.IsValid() || !textureHandle.IsValid())
        return;

    const Resource::MaterialData *material = m_materialManager->GetMaterial(materialHandle);
    if (!material)
        return;

    auto &transform = registry.GetComponent<ECS::TransformComponent>(entity);

    float depth = CalculateDepth(transform.position, cameraPos);

    // 构建 ObjectConstants
    ObjectConstants objCB;
    XMMATRIX world = transform.GetMatrix();
    XMMATRIX worldInvTranspose = XMMatrixTranspose(XMMatrixInverse(nullptr, world));

    XMStoreFloat4x4(&objCB.World, world);
    XMStoreFloat4x4(&objCB.WorldInvTranspose, worldInvTranspose);
    XMStoreFloat4x4(&objCB.PrevWorld, world);
    objCB.MaterialIndex = m_materialManager->GetGPUIndex(materialHandle);
    objCB.ReceiveShadow = 0;

    D3D12_GPU_VIRTUAL_ADDRESS objectCBAddress =
        m_frameResourceManager->AllocateObjectCB(&objCB, sizeof(ObjectConstants));

    // 构建 RenderItem
    RenderItem item;
    item.geometryHandle = handle;
    item.materialHandle = materialHandle;
    item.worldMatrix = transform.GetMatrix();
    item.objectCBAddress = objectCBAddress;
    item.depth = depth;
    item.sortKey = BuildSortKey(materialHandle.index, depth, isTransparent);
    item.textureSRV = m_textureManager->GetSRV(textureHandle);

    outQueue.Add(item);
}

// ============================================================================
// 辅助方法
// ============================================================================

float RenderItemBuilder::CalculateDepth(const DirectX::XMFLOAT3 &pos, const DirectX::XMFLOAT3 &cameraPos) const {
    float dx = pos.x - cameraPos.x;
    float dy = pos.y - cameraPos.y;
    float dz = pos.z - cameraPos.z;
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

uint64_t RenderItemBuilder::BuildSortKey(uint32_t materialIndex, float depth, bool isTransparent) const {
    // 位布局：
    //   Bits 63:      透明标志（1 = 透明）
    //   Bits 62-48:   保留
    //   Bits 47-32:   材质索引（16 位）
    //   Bits 31-0:    深度值
    uint64_t transparentBit = isTransparent ? (1ULL << 63) : 0;
    uint64_t materialPart = (static_cast<uint64_t>(materialIndex) & 0xFFFF) << 32;

    uint32_t depthInt = static_cast<uint32_t>(depth * 1000.0f);

    if (isTransparent) {
        depthInt = 0xFFFFFFFF - depthInt;
    }

    return transparentBit | materialPart | depthInt;
}

// 显式实例化模板（避免链接错误）
template void RenderItemBuilder::BuildRenderItem<ECS::MeshComponent>(ECS::Registry &, ECS::Entity, ECS::MeshComponent &,
                                                                     const LODResult &, const DirectX::XMFLOAT3 &,
                                                                     RenderQueue &, bool);

template void RenderItemBuilder::BuildRenderItem<ECS::TransparentMeshComponent>(ECS::Registry &, ECS::Entity,
                                                                                ECS::TransparentMeshComponent &,
                                                                                const LODResult &,
                                                                                const DirectX::XMFLOAT3 &,
                                                                                RenderQueue &, bool);

} // namespace DX12Engine::Renderer