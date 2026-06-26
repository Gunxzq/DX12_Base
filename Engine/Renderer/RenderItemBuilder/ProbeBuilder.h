#pragma once

#include "ECS/Core/Registry.h"
#include "OpaqueRenderItem.h"
#include "ProbeCaptureInfo.h"
#include "Renderer/Core/LODSystem.h"
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

    void SetLODSystem(LODSystem *lodSystem) { m_lodSystem = lodSystem; }

    void Build(const ProbeCaptureInfo *probes, uint32_t probeCount, ECS::Registry &registry,
               TRenderQueue<OpaqueRenderItem> *outputQueues);

private:
    FrameResourceManager *m_frameResourceManager;
    Resource::MaterialManager *m_materialManager;
    Resource::TextureManager *m_textureManager;
    LODSystem *m_lodSystem = nullptr;
};

} // namespace DX12Engine::Renderer
