#pragma once

#include "ECS/Core/Registry.h"
#include "OpaqueRenderItem.h"
#include "ProbeCaptureInfo.h"
#include "Renderer/Core/CullingUtil.h"
#include "Renderer/Core/LODSystem.h"
#include "Renderer/Scene/Struct/Frustum.h"
#include "TRenderQueue.h"

namespace DX12Engine::Resource {
class MaterialManager;
class TextureManager;
} // namespace DX12Engine::Resource

namespace DX12Engine::Renderer {

class FrameResourceManager;

class ProbeBuilder {
public:
    ProbeBuilder(FrameResourceManager *frameResources, Resource::MaterialManager *materialManager,
                 Resource::TextureManager *textureManager);

    void SetFrustum(const Frustum *frustum) { m_frustum = frustum; }
    void SetCameraPos(const DirectX::XMFLOAT3 &pos) { m_cameraPos = pos; }
    void SetLODSystem(const LODSystem *system) { m_lodSystem = system; }

    void Build(const ProbeCaptureInfo *probes, uint32_t probeCount, ECS::Registry &registry,
               TRenderQueue<OpaqueRenderItem> *outputQueues);

private:
    FrameResourceManager *m_frameResourceManager;
    Resource::MaterialManager *m_materialManager;
    Resource::TextureManager *m_textureManager;

    const Frustum *m_frustum = nullptr;
    const LODSystem *m_lodSystem = nullptr;
    DirectX::XMFLOAT3 m_cameraPos = {};
};

} // namespace DX12Engine::Renderer
