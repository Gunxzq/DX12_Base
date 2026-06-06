// TerrainRenderItemBuilder.cpp
#include "TerrainRenderItemBuilder.h"
#include "ECS/Core/Components.h"
#include "Renderer/FrameResources/FrameResourceManager.h"
#include "Resource/Texture/TextureManager.h"

using namespace DX12Engine::Renderer;
using namespace DX12Engine::Resource;
using namespace DX12Engine::ECS;

namespace DX12Engine::Renderer {

TerrainRenderItemBuilder::TerrainRenderItemBuilder(FrameResourceManager *frameResources, TextureManager *textureManager)
    : m_frameResourceManager(frameResources), m_textureManager(textureManager) {}

void TerrainRenderItemBuilder::BuildTyped(ECS::Registry &registry, TRenderQueue<TerrainRenderItem> &outQueue) {
    outQueue.Clear();

    if (!m_cullingResult) {
        return;
    }

    for (auto entity : m_cullingResult->visibleEntities) {
        // 检查是否有地形组件
        auto *terrainComp = registry.TryGetComponent<TerrainComponent>(entity);
        if (!terrainComp)
            continue;

        // 直接从 TerrainComponent 获取 GeometryHandle（不使用 LOD）
        GeometryHandle geoHandle = terrainComp->geometryHandle;
        if (!geoHandle.IsValid())
            continue;

        // 获取纹理 SRV
        D3D12_GPU_DESCRIPTOR_HANDLE heightMapSRV = {};
        D3D12_GPU_DESCRIPTOR_HANDLE albedoSRV = {};
        D3D12_GPU_DESCRIPTOR_HANDLE normalSRV = {};

        if (terrainComp->heightMapHandle.IsValid()) {
            heightMapSRV = m_textureManager->GetSRV(terrainComp->heightMapHandle);
        }
        if (terrainComp->albedoHandle.IsValid()) {
            albedoSRV = m_textureManager->GetSRV(terrainComp->albedoHandle);
        }
        if (terrainComp->normalHandle.IsValid()) {
            normalSRV = m_textureManager->GetSRV(terrainComp->normalHandle);
        }

        // 构建渲染项
        TerrainRenderItem item;
        item.geometryHandle = geoHandle;
        item.objectCBAddress = 0; // 由 TerrainManager 在分配时填充
        item.heightMapSRV = heightMapSRV;
        item.albedoSRV = albedoSRV;
        item.normalSRV = normalSRV;
        item.heightScale = terrainComp->heightScale;
        item.heightOffset = terrainComp->heightOffset;
        item.tessellationFactor = terrainComp->tessellationFactor;
        item.tessellationDistanceMin = terrainComp->tessellationDistanceMin;
        item.tessellationDistanceMax = terrainComp->tessellationDistanceMax;
        item.materialIndex = terrainComp->materialIndex;

        outQueue.Add(item);
    }
}

} // namespace DX12Engine::Renderer