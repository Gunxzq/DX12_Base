#pragma once

#include "BillboardRenderItem.h"
#include "ECS/Core/Registry.h"
#include "IRenderItemBuilder.h"
#include "Renderer/Core/CullingUtil.h"
#include "Renderer/Core/LODSystem.h"
#include "Renderer/Scene/Struct/Frustum.h"
#include "TRenderQueue.h"

namespace DX12Engine::Resource {
class TextureManager;
class MaterialManager;
} // namespace DX12Engine::Resource

namespace DX12Engine::Renderer {

class FrameResourceManager;

class BillboardRenderItemBuilder : public TRenderItemBuilder<TRenderQueue<BillboardRenderItem>> {
public:
    BillboardRenderItemBuilder(FrameResourceManager *frameResources, Resource::TextureManager *textureManager,
                               Resource::MaterialManager *materialManager);

    void SetFrustum(const Frustum *frustum) { m_frustum = frustum; }
    void SetCameraPos(const DirectX::XMFLOAT3 &pos) { m_cameraPos = pos; }

    void BuildTyped(ECS::Registry &registry, TRenderQueue<BillboardRenderItem> &outQueue) override;

    const char *GetName() const override { return "BillboardRenderItemBuilder"; }

private:
    FrameResourceManager *m_frameResourceManager;
    Resource::TextureManager *m_textureManager;
    Resource::MaterialManager *m_materialManager;

    const Frustum *m_frustum = nullptr;
    DirectX::XMFLOAT3 m_cameraPos = {};
};

} // namespace DX12Engine::Renderer
