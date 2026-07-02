#pragma once

#include "ECS/Core/Registry.h"
#include "IRenderItemBuilder.h"
#include "Renderer/Core/CullingUtil.h"
#include "Renderer/Scene/Struct/Frustum.h"
#include "TRenderQueue.h"
#include "TerrainRenderItem.h"

namespace DX12Engine::Resource {
class TextureManager;
} // namespace DX12Engine::Resource

namespace DX12Engine::Renderer {

class FrameResourceManager;

class TerrainRenderItemBuilder : public TRenderItemBuilder<TRenderQueue<TerrainRenderItem>> {
public:
    TerrainRenderItemBuilder(FrameResourceManager *frameResources, Resource::TextureManager *textureManager);

    void SetFrustum(const Frustum *frustum) { m_frustum = frustum; }

    void BuildTyped(ECS::Registry &registry, TRenderQueue<TerrainRenderItem> &outQueue) override;

    const char *GetName() const override { return "TerrainRenderItemBuilder"; }

private:
    FrameResourceManager *m_frameResourceManager;
    Resource::TextureManager *m_textureManager;

    const Frustum *m_frustum = nullptr;
};

} // namespace DX12Engine::Renderer
