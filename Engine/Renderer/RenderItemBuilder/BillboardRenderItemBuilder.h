#pragma once

#include "BillboardRenderItem.h"
#include "ECS/Core/Registry.h"
#include "IRenderItemBuilder.h"
#include "Renderer/Core/CullingSystem.h"
#include "Renderer/Core/LODSystem.h"
#include "TRenderQueue.h"

namespace DX12Engine::Resource {
class TextureManager;
class MaterialManager;
} // namespace DX12Engine::Resource

namespace DX12Engine::Renderer {

class FrameResourceManager;

// ============================================================================
// 公告牌渲染项构建器 - 收集公告牌实体并构建渲染项
// ============================================================================
class BillboardRenderItemBuilder : public TRenderItemBuilder<TRenderQueue<BillboardRenderItem>> {
public:
    BillboardRenderItemBuilder(FrameResourceManager *frameResources, Resource::TextureManager *textureManager,
                               Resource::MaterialManager *materialManager);

    // 设置每帧数据
    void SetCullingResult(const CullingResult *result) { m_cullingResult = result; }
    void SetLODResult(const LODResult *result) { m_lodResult = result; }
    void SetCameraPosition(const DirectX::XMFLOAT3 &cameraPos) { m_cameraPos = cameraPos; }

    void BuildTyped(ECS::Registry &registry, TRenderQueue<BillboardRenderItem> &outQueue) override;

    const char *GetName() const override { return "BillboardRenderItemBuilder"; }

private:
    FrameResourceManager *m_frameResourceManager;
    Resource::TextureManager *m_textureManager;
    Resource::MaterialManager *m_materialManager;

    const CullingResult *m_cullingResult = nullptr;
    const LODResult *m_lodResult = nullptr;
    DirectX::XMFLOAT3 m_cameraPos = {};
};

} // namespace DX12Engine::Renderer