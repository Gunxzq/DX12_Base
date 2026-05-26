#pragma once

#include "ECS/Core/Registry.h"
#include "Renderer/Core/LODConfig.h"
#include "Renderer/Scene/CameraManager.h"
#include "Resource/Asset/LODMesh.h"
#include "Resource/Manager/GeometryResourceManager.h"
#include "Resource/Struct/LODMeshHandle.h"
#include <unordered_map>

namespace DX12Engine::Renderer {

struct LODResult {
    std::unordered_map<ECS::Entity, Resource::GeometryHandle> handleMap;

    void Clear() { handleMap.clear(); }
    void SetHandle(ECS::Entity entity, Resource::GeometryHandle handle) { handleMap[entity] = handle; }
    Resource::GeometryHandle GetHandle(ECS::Entity entity) const {
        auto it = handleMap.find(entity);
        return it != handleMap.end() ? it->second : Resource::GeometryHandle::Invalid();
    }

    // size
    size_t Size() const { return handleMap.size(); }
};

// ============================================================================
// LOD 系统 - 负责为每个实体选择合适的几何体精度
// ============================================================================

class LODSystem {
public:
    LODSystem() = default;
    ~LODSystem() = default;

    // 禁止拷贝
    LODSystem(const LODSystem &) = delete;
    LODSystem &operator=(const LODSystem &) = delete;

    void SetLODConfig(const LODConfig &config) { m_lodConfig = config; }
    void SetCameraManager(CameraManager *mgr) { m_cameraManager = mgr; }
    void SetGeometryManager(Resource::GeometryResourceManager *mgr) { m_geometryManager = mgr; }

    // ========================================================================
    // LODMesh 管理（临时，后续可由 AssetManager 接管）
    // ========================================================================

    Resource::LODMeshHandle RegisterLODMesh(const Resource::LODMesh &lodMesh);
    void RegisterLODMesh(Resource::LODMeshHandle handle, const Resource::LODMesh &lodMesh);
    void UnregisterLODMesh(Resource::LODMeshHandle handle);
    const Resource::LODMesh *GetLODMesh(Resource::LODMeshHandle handle) const;

    void Execute(ECS::Registry &registry, LODResult &outResult);

private:
    // ========================================================================
    // 辅助方法
    // ========================================================================

    float CalculateDistance(const DirectX::XMFLOAT3 &pos, const DirectX::XMFLOAT3 &cameraPos) const;

    Resource::GeometryHandle ResolveGeometryHandle(const Resource::LODMesh &lodMesh, uint32_t configIndex) const;

    LODConfig m_lodConfig;
    CameraManager *m_cameraManager = nullptr;
    Resource::GeometryResourceManager *m_geometryManager = nullptr;

    // LODMesh 存储（临时，后续可由 AssetManager 接管）
    std::unordered_map<Resource::LODMeshHandle, Resource::LODMesh> m_lodMeshes;
};

} // namespace DX12Engine::Renderer