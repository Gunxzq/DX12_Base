#pragma once

#include "ECS/Core/Registry.h"
#include "IRenderItemBuilder.h"
#include "Renderer/Core/CullingSystem.h"
#include "Renderer/Core/LODSystem.h"
#include "TRenderQueue.h"
#include "TransparentRenderItem.h"

// 前向声明
namespace DX12Engine::Resource {
class MaterialManager;
class TextureManager;
} // namespace DX12Engine::Resource

namespace DX12Engine::Renderer {

class CameraManager;
class FrameResourceManager;

// ============================================================================
// 透明物体渲染项构建器
// 透明物体需要从远到近排序，因此需要相机来计算距离
// ============================================================================
class TransparentRenderItemBuilder : public TRenderItemBuilder<TRenderQueue<TransparentRenderItem>> {
public:
    TransparentRenderItemBuilder(FrameResourceManager *frameResources, Resource::MaterialManager *materialManager,
                                 Resource::TextureManager *textureManager, CameraManager *cameraManager);

    // 设置每帧数据
    void SetCullingResult(const CullingResult *result) { m_cullingResult = result; }
    void SetLODResult(const LODResult *result) { m_lodResult = result; }

    void BuildTyped(ECS::Registry &registry, TRenderQueue<TransparentRenderItem> &outQueue) override;

private:
    float CalculateDepth(const DirectX::XMFLOAT3 &pos, const DirectX::XMFLOAT3 &cameraPos) const;

    FrameResourceManager *m_frameResourceManager;
    Resource::MaterialManager *m_materialManager;
    Resource::TextureManager *m_textureManager;
    CameraManager *m_cameraManager;

    const CullingResult *m_cullingResult = nullptr;
    const LODResult *m_lodResult = nullptr;
};

} // namespace DX12Engine::Renderer
