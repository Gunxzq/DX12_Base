#include "TerrainRenderItemBuilder.h"
#include "ECS/Core/Components.h"
#include "Renderer/FrameResources/FrameResourceManager.h"
#include "Renderer/Scene/TerrainManager/TerrainManager.h"
#include "Resource/Texture/TextureManager.h"

using namespace DX12Engine::Renderer;
using namespace DX12Engine::Resource;
using namespace DX12Engine::ECS;

namespace DX12Engine::Renderer {

TerrainRenderItemBuilder::TerrainRenderItemBuilder(FrameResourceManager *frameResources, TextureManager *textureManager)
    : m_frameResourceManager(frameResources), m_textureManager(textureManager) {}

void TerrainRenderItemBuilder::BuildTyped(ECS::Registry &registry, TRenderQueue<TerrainRenderItem> &outQueue) {
    outQueue.Clear();

    auto &terrainMgr = TerrainManager::GetInstance();

    UINT blockIndex = 0;
    auto view = registry.view<TerrainComponent, TransformComponent>();
    for (auto entity : view) {
        auto &terrainComp = view.get<TerrainComponent>(entity);
        auto &transform = view.get<TransformComponent>(entity);

        GeometryHandle geoHandle = terrainComp.geometryHandle;
        if (!geoHandle.IsValid())
            continue;

        D3D12_GPU_DESCRIPTOR_HANDLE texTableSRV = {};
        if (terrainComp.heightMapHandle.IsValid() && terrainComp.albedoHandle.IsValid() && terrainComp.normalHandle.IsValid()) {
            texTableSRV = m_textureManager->GetSRV(terrainComp.heightMapHandle);
        }

        D3D12_GPU_VIRTUAL_ADDRESS objectCBAddress = terrainMgr.GetTerrainBlockAddress(blockIndex);

        TerrainRenderItem item;
        item.geometryHandle = geoHandle;
        item.objectCBAddress = objectCBAddress;
        item.texTableSRV = texTableSRV;
        item.heightScale = terrainComp.heightScale;
        item.heightOffset = terrainComp.heightOffset;
        item.tessellationFactor = terrainComp.tessellationFactor;
        item.tessellationDistanceMin = terrainComp.tessellationDistanceMin;
        item.tessellationDistanceMax = terrainComp.tessellationDistanceMax;
        item.materialIndex = terrainComp.materialIndex;

        outQueue.Add(item);
        blockIndex++;
    }
}

} // namespace DX12Engine::Renderer
