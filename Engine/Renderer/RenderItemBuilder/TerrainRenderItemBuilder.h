#pragma once

#include "ECS/Core/Registry.h"
#include "IRenderItemBuilder.h"
#include "Renderer/Core/CullingSystem.h"
#include "TRenderQueue.h"
#include "TerrainRenderItem.h"

namespace DX12Engine::Resource {
class TextureManager;
} // namespace DX12Engine::Resource

namespace DX12Engine::Renderer {

class FrameResourceManager;

// ============================================================================
// 地形渲染项构建器 - 收集地形实体并构建渲染项
// ============================================================================
class TerrainRenderItemBuilder : public TRenderItemBuilder<TRenderQueue<TerrainRenderItem>> {
public:
    TerrainRenderItemBuilder(FrameResourceManager *frameResources, Resource::TextureManager *textureManager);

    // 设置每帧数据
    void SetCullingResult(const CullingResult *result) { m_cullingResult = result; }

    void BuildTyped(ECS::Registry &registry, TRenderQueue<TerrainRenderItem> &outQueue) override;

    const char *GetName() const override { return "TerrainRenderItemBuilder"; }

private:
    FrameResourceManager *m_frameResourceManager;
    Resource::TextureManager *m_textureManager;

    const CullingResult *m_cullingResult = nullptr;
};

} // namespace DX12Engine::Renderer