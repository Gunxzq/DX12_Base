#pragma once

#include "ECS/Core/Registry.h"
#include "IRenderItemBuilder.h"
#include "Renderer/Core/CullingSystem.h"
#include "Renderer/Core/LODSystem.h"
#include "Renderer/FrameResources/Struct/FrameResourceTypes.h"
#include "SkinnedRenderItem.h"
#include "TRenderQueue.h"

#include <vector>

namespace DX12Engine::Resource {
class MaterialManager;
class SkeletonManager;
} // namespace DX12Engine::Resource

namespace DX12Engine::Renderer {

class FrameResourceManager;

class SkinnedRenderItemBuilder : public TRenderItemBuilder<TRenderQueue<SkinnedRenderItem>> {
public:
    SkinnedRenderItemBuilder(FrameResourceManager *frameResources, Resource::MaterialManager *materialManager,
                             Resource::SkeletonManager *skeletonManager);

    void SetCullingResult(const CullingResult *result) { m_cullingResult = result; }
    void SetLODResult(const LODResult *result) { m_lodResult = result; }

    void BuildTyped(ECS::Registry &registry, TRenderQueue<SkinnedRenderItem> &outQueue) override;

private:
    FrameResourceManager *m_frameResourceManager;
    Resource::MaterialManager *m_materialManager;
    Resource::SkeletonManager *m_skeletonManager;

    const CullingResult *m_cullingResult = nullptr;
    const LODResult *m_lodResult = nullptr;

    /// 临时骨骼矩阵缓存（复用分配，避免每帧 vector 重新分配）
    std::vector<DirectX::XMFLOAT4X4> m_boneTransformCache;
};

} // namespace DX12Engine::Renderer
