#include "OpaqueRenderItemBuilder.h"
#include "ECS/Core/Components.h"
#include "Renderer/FrameResources/FrameResourceManager.h"
#include "Resource/Manager/MaterialManager.h"
#include "Resource/Texture/TextureManager.h"

using namespace DX12Engine::Renderer;
using namespace DX12Engine::Resource;
using namespace DX12Engine::ECS;

namespace DX12Engine::Renderer {

OpaqueRenderItemBuilder::OpaqueRenderItemBuilder(FrameResourceManager *frameResources, MaterialManager *materialManager,
                                                 TextureManager *textureManager)
    : m_frameResourceManager(frameResources), m_materialManager(materialManager), m_textureManager(textureManager) {}

void OpaqueRenderItemBuilder::BuildTyped(ECS::Registry &registry, TRenderQueue<OpaqueRenderItem> &outQueue) {
    outQueue.Clear();

    for (auto entity : m_cullingResult->visibleEntities) {
        // 只处理有 MeshComponent 的实体（透明实体由 TransparentRenderItemBuilder 处理）
        auto *meshComp = registry.TryGetComponent<MeshComponent>(entity);
        if (!meshComp)
            continue;

        // 从 LODResult 获取几何体（LOD 系统已选择好）
        GeometryHandle geoHandle = m_lodResult->GetHandle(entity);
        if (!geoHandle.IsValid())
            continue;

        auto *transform = registry.TryGetComponent<TransformComponent>(entity);
        if (!transform)
            continue;

        // 获取材质和纹理
        MaterialHandle materialHandle = meshComp->materialHandle;
        TextureHandle textureHandle = meshComp->textureHandle;
        if (!materialHandle.IsValid() || !textureHandle.IsValid())
            continue;

        // 构建 ObjectConstants
        ObjectConstants objCB;
        XMMATRIX world = transform->GetMatrix();
        XMMATRIX worldInvTranspose = XMMatrixTranspose(XMMatrixInverse(nullptr, world));

        XMStoreFloat4x4(&objCB.World, world);
        XMStoreFloat4x4(&objCB.WorldInvTranspose, worldInvTranspose);
        objCB.MaterialIndex = m_materialManager->GetGPUIndex(materialHandle);
        objCB.ReceiveShadow = meshComp->receivesShadow ? 1 : 0;

        // 分配常量缓冲
        D3D12_GPU_VIRTUAL_ADDRESS objectCBAddress =
            m_frameResourceManager->AllocateObjectCB(&objCB, sizeof(ObjectConstants));

        // 构建渲染项
        OpaqueRenderItem item;
        item.geometryHandle = geoHandle;
        item.worldMatrix = world;
        item.objectCBAddress = objectCBAddress;
        item.materialIndex = objCB.MaterialIndex;
        item.textureSRV = m_textureManager->GetSRV(textureHandle);

        outQueue.Add(item);
    }
}

} // namespace DX12Engine::Renderer