#include "TransparentRenderItemBuilder.h"
#include "ECS/Core/Components.h"
#include "Renderer/FrameResources/FrameResourceManager.h"
#include "Renderer/Scene/CameraManager.h"
#include "Resource/Manager/MaterialManager.h"
#include "Resource/Texture/TextureManager.h"

using namespace DX12Engine::Renderer;
using namespace DX12Engine::Resource;
using namespace DX12Engine::ECS;

namespace DX12Engine::Renderer {

TransparentRenderItemBuilder::TransparentRenderItemBuilder(FrameResourceManager *frameResources,
                                                           MaterialManager *materialManager,
                                                           TextureManager *textureManager, CameraManager *cameraManager)
    : m_frameResourceManager(frameResources), m_materialManager(materialManager), m_textureManager(textureManager),
      m_cameraManager(cameraManager) {}

void TransparentRenderItemBuilder::BuildTyped(ECS::Registry &registry, TRenderQueue<TransparentRenderItem> &outQueue) {
    outQueue.Clear();

    // 获取相机位置，用于计算深度
    DirectX::XMFLOAT3 cameraPos = {0.0f, 0.0f, 0.0f};
    if (m_cameraManager) {
        const auto &camera = m_cameraManager->GetMainCamera();
        cameraPos = camera.Position;
    }

    for (auto entity : m_cullingResult->visibleEntities) {
        // 只处理有 TransparentMeshComponent 的实体
        auto *transparentComp = registry.TryGetComponent<TransparentMeshComponent>(entity);
        if (!transparentComp)
            continue;

        // 从 LODResult 获取几何体
        GeometryHandle geoHandle = m_lodResult->GetHandle(entity);
        if (!geoHandle.IsValid())
            continue;

        auto *transform = registry.TryGetComponent<TransformComponent>(entity);
        if (!transform)
            continue;

        // 获取材质和纹理
        MaterialHandle materialHandle = transparentComp->materialHandle;
        TextureHandle textureHandle = transparentComp->textureHandle;
        if (!materialHandle.IsValid() || !textureHandle.IsValid())
            continue;

        // TODO(StaticComponent): 静态优化暂未启用，全部走动态路径
        // auto *staticComp = registry.TryGetComponent<StaticComponent>(entity);
        // bool isStatic = (staticComp != nullptr);

        // 计算到相机的距离（用于远到近排序）
        float depth = CalculateDepth(transform->position, cameraPos);

        // 构建 ObjectConstants
        ObjectConstants objCB;
        XMMATRIX world;

        // 每帧重新计算 World 矩阵（静态优化禁用后统一路径）
        world = transform->GetMatrix();
        XMMATRIX worldInvTranspose = XMMatrixTranspose(XMMatrixInverse(nullptr, world));
        XMStoreFloat4x4(&objCB.World, world);
        XMStoreFloat4x4(&objCB.WorldInvTranspose, worldInvTranspose);

        objCB.MaterialIndex = m_materialManager->GetGPUIndex(materialHandle);
        objCB.ReceiveShadow = 0; // 透明物体通常不接收阴影

        // TODO(StaticComponent): 静态优化暂未启用，全部走动态路径
        D3D12_GPU_VIRTUAL_ADDRESS objectCBAddress =
            m_frameResourceManager->AllocateObjectCB(&objCB, sizeof(ObjectConstants));

        // 构建渲染项
        TransparentRenderItem item;
        item.geometryHandle = geoHandle;
        item.worldMatrix = world;
        item.objectCBAddress = objectCBAddress;
        item.materialIndex = objCB.MaterialIndex;
        item.textureSRV = m_textureManager->GetSRV(textureHandle);
        item.depth = depth;

        outQueue.Add(item);
    }

    // 透明物体：从远到近排序（depth 降序）
    outQueue.Sort([](const TransparentRenderItem &a, const TransparentRenderItem &b) { return a.depth > b.depth; });
}

float TransparentRenderItemBuilder::CalculateDepth(const DirectX::XMFLOAT3 &pos,
                                                   const DirectX::XMFLOAT3 &cameraPos) const {
    float dx = pos.x - cameraPos.x;
    float dy = pos.y - cameraPos.y;
    float dz = pos.z - cameraPos.z;
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

} // namespace DX12Engine::Renderer
