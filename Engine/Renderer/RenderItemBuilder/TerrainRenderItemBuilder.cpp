// TerrainRenderItemBuilder.cpp
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

    if (!m_cullingResult) {
        OutputDebugStringW(L"[TerrainBuilder] No culling result, skip\n");
        return;
    }

    auto &terrainMgr = TerrainManager::GetInstance();

    UINT blockIndex = 0;
    for (auto entity : m_cullingResult->visibleEntities) {
        // 检查是否有地形组件
        auto *terrainComp = registry.TryGetComponent<TerrainComponent>(entity);
        if (!terrainComp)
            continue;

        // 直接从 TerrainComponent 获取 GeometryHandle（不使用 LOD）
        GeometryHandle geoHandle = terrainComp->geometryHandle;
        if (!geoHandle.IsValid())
            continue;

        // 获取纹理描述符表起始 SRV
        // AllocateConsecutive(3) 保证 [0]=高度图, [1]=漫反射, [2]=法线贴图 在堆中连续
        // 根签名 texTable 有 3 个 SRV，三者都必须有效才能渲染
        D3D12_GPU_DESCRIPTOR_HANDLE texTableSRV = {};
        if (terrainComp->heightMapHandle.IsValid() && terrainComp->albedoHandle.IsValid() && terrainComp->normalHandle.IsValid()) {
            texTableSRV = m_textureManager->GetSRV(terrainComp->heightMapHandle);
        }

        // LightManager 模式：从 TerrainManager 查询已上传的 GPU 地址
        // Immediate 回调中已完成分配+上传，这里只需按索引查询
        D3D12_GPU_VIRTUAL_ADDRESS objectCBAddress = terrainMgr.GetTerrainBlockAddress(blockIndex);

        // 构建渲染项
        TerrainRenderItem item;
        item.geometryHandle = geoHandle;
        item.objectCBAddress = objectCBAddress;
        item.texTableSRV = texTableSRV;
        item.heightScale = terrainComp->heightScale;
        item.heightOffset = terrainComp->heightOffset;
        item.tessellationFactor = terrainComp->tessellationFactor;
        item.tessellationDistanceMin = terrainComp->tessellationDistanceMin;
        item.tessellationDistanceMax = terrainComp->tessellationDistanceMax;
        item.materialIndex = terrainComp->materialIndex;

        outQueue.Add(item);
        blockIndex++;
    }
}

} // namespace DX12Engine::Renderer