#pragma once

#include "ECS/Core/Registry.h"
#include "Renderer/Core/LODConfig.h"
#include "Renderer/Scene/CameraManager.h"
#include "Renderer/Core/LODMesh.h"
#include "Resource/Manager/GeometryResourceManager.h"
#include "Resource/Struct/LODMeshHandle.h"

namespace DX12Engine::Renderer {

// ============================================================================
// LOD 系统 — 仅提供 LODMesh 存储（供 Builder/PickLOD 查询）
// ============================================================================

class LODSystem {
public:
    LODSystem() = default;
    ~LODSystem() = default;

    LODSystem(const LODSystem &) = delete;
    LODSystem &operator=(const LODSystem &) = delete;

    void SetLODConfig(const LODConfig &config) { m_lodConfig = config; }
    void SetCameraManager(CameraManager *mgr) { m_cameraManager = mgr; }
    void SetGeometryManager(Resource::GeometryResourceManager *mgr) { m_geometryManager = mgr; }

    // ========================================================================
    // LODMesh 管理
    // ========================================================================

    Resource::LODMeshHandle RegisterLODMesh(const Resource::LODMesh &lodMesh);
    void RegisterLODMesh(Resource::LODMeshHandle handle, const Resource::LODMesh &lodMesh);
    void UnregisterLODMesh(Resource::LODMeshHandle handle);
    const Resource::LODMesh *GetLODMesh(Resource::LODMeshHandle handle) const;

    /// 获取当前 LOD 配置（供 PickLOD 使用）
    const LODConfig &GetLODConfig() const { return m_lodConfig; }

private:
    LODConfig m_lodConfig;
    CameraManager *m_cameraManager = nullptr;
    Resource::GeometryResourceManager *m_geometryManager = nullptr;

    std::unordered_map<Resource::LODMeshHandle, Resource::LODMesh> m_lodMeshes;
};

} // namespace DX12Engine::Renderer
