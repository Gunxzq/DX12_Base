#include "LODSystem.h"

using namespace DX12Engine::Resource;

namespace DX12Engine::Renderer {

Resource::LODMeshHandle LODSystem::RegisterLODMesh(const Resource::LODMesh &lodMesh) {
    Resource::LODMeshHandle handle;
    handle.index = static_cast<uint32_t>(m_lodMeshes.size());
    handle.generation = 1;
    m_lodMeshes[handle] = lodMesh;
    return handle;
}

void LODSystem::RegisterLODMesh(Resource::LODMeshHandle handle, const Resource::LODMesh &lodMesh) {
    m_lodMeshes[handle] = lodMesh;
}

void LODSystem::UnregisterLODMesh(Resource::LODMeshHandle handle) { m_lodMeshes.erase(handle); }

const Resource::LODMesh *LODSystem::GetLODMesh(Resource::LODMeshHandle handle) const {
    auto it = m_lodMeshes.find(handle);
    if (it != m_lodMeshes.end())
        return &it->second;
    return nullptr;
}

} // namespace DX12Engine::Renderer
