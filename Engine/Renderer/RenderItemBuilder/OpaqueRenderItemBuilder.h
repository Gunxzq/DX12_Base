#pragma once

#include "ECS/Core/Registry.h"
#include "IRenderItemBuilder.h"
#include "OpaqueRenderItem.h"
#include "Renderer/Core/CullingSystem.h"
#include "Renderer/Core/LODSystem.h"
#include "TRenderQueue.h"

// 前向声明
namespace DX12Engine::Resource {
class MaterialManager;
class TextureManager;
} // namespace DX12Engine::Resource

namespace DX12Engine::Renderer {

class FrameResourceManager;

class OpaqueRenderItemBuilder : public TRenderItemBuilder<TRenderQueue<OpaqueRenderItem>> {
public:
    OpaqueRenderItemBuilder(FrameResourceManager *frameResources, Resource::MaterialManager *materialManager,
                            Resource::TextureManager *textureManager);

    // 设置每帧数据
    void SetCullingResult(const CullingResult *result) { m_cullingResult = result; }
    void SetLODResult(const LODResult *result) { m_lodResult = result; }

    void BuildTyped(ECS::Registry &registry, TRenderQueue<OpaqueRenderItem> &outQueue) override;

private:
    FrameResourceManager *m_frameResourceManager;
    Resource::MaterialManager *m_materialManager;
    Resource::TextureManager *m_textureManager;

    const CullingResult *m_cullingResult = nullptr;
    const LODResult *m_lodResult = nullptr;
};

} // namespace DX12Engine::Renderer