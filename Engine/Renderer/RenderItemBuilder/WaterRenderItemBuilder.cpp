#include "WaterRenderItemBuilder.h"
#include "ECS/Core/Components.h"
#include "Renderer/FrameResources/FrameResourceManager.h"
#include "Renderer/Material/MaterialManager.h"
#include "Renderer/Scene/CameraManager.h"
#include "Resource/Manager/GeometryResourceManager.h"
#include <DirectXMath.h>

using namespace DX12Engine::Renderer;
using namespace DX12Engine::Resource;
using namespace DX12Engine::ECS;

namespace DX12Engine::Renderer {

WaterRenderItemBuilder::WaterRenderItemBuilder(FrameResourceManager *frameResources, MaterialManager *materialManager,
                                               CameraManager *cameraManager)
    : m_frameResourceManager(frameResources), m_materialManager(materialManager), m_cameraManager(cameraManager) {}

void WaterRenderItemBuilder::BuildTyped(ECS::Registry &registry, TRenderQueue<WaterRenderItem> &outQueue) {
    outQueue.Clear();

    if (!m_frustum)
        return;

    auto view = registry.view<WaterComponent, MeshComponent, TransformComponent, TransparentTag>();
    for (auto entity : view) {
        auto &waterComp = view.get<WaterComponent>(entity);
        auto &meshComp = view.get<MeshComponent>(entity);
        auto &transform = view.get<TransformComponent>(entity);

        if (!waterComp.IsValid() || !meshComp.IsValid())
            continue;

        // 通过 LODSystem 获取 GeometryHandle
        GeometryHandle geoHandle;
        if (m_lodSystem && meshComp.lodMeshHandle.IsValid()) {
            const auto *lodMesh = m_lodSystem->GetLODMesh(meshComp.lodMeshHandle);
            if (lodMesh && !lodMesh->lodChain.empty())
                geoHandle = lodMesh->lodChain[0];
        }
        if (!geoHandle.IsValid())
            continue;

        DirectX::XMMATRIX world = transform.GetMatrix();

        // 距离剔除
        if (!FrustumCull(meshComp.localBounds, world, *m_frustum))
            continue;

        // 计算深度
        float dx = transform.position.x - m_cameraPos.x;
        float dy = transform.position.y - m_cameraPos.y;
        float dz = transform.position.z - m_cameraPos.z;
        float depth = dx * dx + dy * dy + dz * dz;

        uint32_t materialIdx = m_materialManager->GetGPUIndex(waterComp.materialHandle);

        WaterRenderItem item;
        item.geometryHandle = geoHandle;
        item.worldMatrix = world;
        item.objectCBAddress = waterComp.objectCBAddress; // 持久 UPLOAD 堆地址
        item.waveParamIndex = waterComp.waveParamIndex;
        item.materialIndex = materialIdx;
        item.depth = depth;
        outQueue.Add(item);
    }
}

} // namespace DX12Engine::Renderer
