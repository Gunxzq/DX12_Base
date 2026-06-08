#include "LODSystem.h"

#include "Common/Common.h"

#include "ECS/Core/Components.h"

using namespace DX12Engine::Renderer;
using namespace DX12Engine::Resource;

namespace DX12Engine::Renderer {

Resource::LODMeshHandle LODSystem::RegisterLODMesh(const Resource::LODMesh &lodMesh) {
    Resource::LODMeshHandle handle;
    handle.index = static_cast<uint32_t>(m_lodMeshes.size());
    handle.generation = 1;
    m_lodMeshes[handle] = lodMesh;
    return handle;
}

/**
 * @brief 注册 LODMesh
 * @param handle LODMesh 句柄
 * @param lodMesh
 * @date 2026-05-26
 */
void LODSystem::RegisterLODMesh(Resource::LODMeshHandle handle, const Resource::LODMesh &lodMesh) {
    m_lodMeshes[handle] = lodMesh;
}

/**
 * @brief 注销 LODMesh
 * @param handle LODMesh 句柄
 * @date 2026-05-26
 */
void LODSystem::UnregisterLODMesh(Resource::LODMeshHandle handle) { m_lodMeshes.erase(handle); }

const Resource::LODMesh *LODSystem::GetLODMesh(Resource::LODMeshHandle handle) const {
    auto it = m_lodMeshes.find(handle);
    if (it != m_lodMeshes.end()) {
        return &it->second;
    }
    return nullptr;
}

// ============================================================================
// 执行
// ============================================================================

void LODSystem::Execute(ECS::Registry &registry, LODResult &outResult) {
    outResult.Clear();
    if (!m_cameraManager) {
        return;
    }

    const auto &camera = m_cameraManager->GetMainCamera();
    const DirectX::XMFLOAT3 &cameraPos = camera.Position;
    uint32_t configLODCount = m_lodConfig.GetLODCount();

    auto view = registry.view<ECS::MeshComponent, ECS::TransformComponent>();

    for (auto entity : view) {
        auto &meshComp = view.get<ECS::MeshComponent>(entity);
        auto &transform = view.get<ECS::TransformComponent>(entity);

        // 1. 获取 LODMesh
        const Resource::LODMesh *lodMesh = GetLODMesh(meshComp.lodMeshHandle);
        if (!lodMesh || !lodMesh->IsValid()) {
            continue;
        }

        // 2. 计算距离（静态物体复用缓存，跳过 sqrt）
        float distance;
        auto *staticComp = registry.TryGetComponent<ECS::StaticComponent>(entity);
        if (staticComp && !staticComp->worldDirty) {
            distance = staticComp->cachedDistanceToCamera;
        } else {
            distance = CalculateDistance(transform.position, cameraPos);
            // 多线程安全性，无法保障
            if (staticComp) {
                staticComp->cachedDistanceToCamera = distance;
            }
        }

        // 3. 获取 LOD 索引（配置请求）
        uint32_t requestedIndex = m_lodConfig.GetLODIndex(distance);

        // 4. 解析实际几何体句柄
        Resource::GeometryHandle geometryHandle = ResolveGeometryHandle(*lodMesh, requestedIndex);
        if (!geometryHandle.IsValid()) {
            // 如果解析失败，尝试使用最高精度
            geometryHandle = lodMesh->GetHighestLOD();
            if (!geometryHandle.IsValid()) {
                continue;
            }
        }

        // 写入临时结构
        outResult.SetHandle(entity, geometryHandle);
    }

    // 透明物体
    auto transparentView = registry.view<ECS::TransparentMeshComponent, ECS::TransformComponent>();
    for (auto entity : transparentView) {
        auto &meshComp = transparentView.get<ECS::TransparentMeshComponent>(entity);
        auto &transform = transparentView.get<ECS::TransformComponent>(entity);

        const Resource::LODMesh *lodMesh = GetLODMesh(meshComp.lodMeshHandle);
        if (!lodMesh || !lodMesh->IsValid())
            continue;

        float distance;
        auto *staticComp = registry.TryGetComponent<ECS::StaticComponent>(entity);
        if (staticComp && !staticComp->worldDirty) {
            distance = staticComp->cachedDistanceToCamera;
        } else {
            distance = CalculateDistance(transform.position, cameraPos);
            // 多线程安全性，无法保障
            if (staticComp) {
                staticComp->cachedDistanceToCamera = distance;
            }
        }
        uint32_t requestedIndex = m_lodConfig.GetLODIndex(distance);
        Resource::GeometryHandle geometryHandle = ResolveGeometryHandle(*lodMesh, requestedIndex);
        if (!geometryHandle.IsValid()) {
            geometryHandle = lodMesh->GetHighestLOD();
            if (!geometryHandle.IsValid())
                continue;
        }
        outResult.SetHandle(entity, geometryHandle);
    }
}

// ============================================================================
// 辅助方法
// ============================================================================

float LODSystem::CalculateDistance(const DirectX::XMFLOAT3 &pos, const DirectX::XMFLOAT3 &cameraPos) const {
    float dx = pos.x - cameraPos.x;
    float dy = pos.y - cameraPos.y;
    float dz = pos.z - cameraPos.z;
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

Resource::GeometryHandle LODSystem::ResolveGeometryHandle(const Resource::LODMesh &lodMesh,
                                                          uint32_t requestedIndex) const {
    uint32_t meshLODCount = static_cast<uint32_t>(lodMesh.GetLODCount());

    // 情况1：资产 LOD 数量足够 → 直接返回请求的等级
    if (requestedIndex < meshLODCount) {
        Resource::GeometryHandle handle = lodMesh.GetLODByIndex(requestedIndex);
        if (handle.IsValid()) {
            return handle;
        }
    }

    // 情况2：请求的等级超出资产范围 → 返回资产的最后一级（最低精度）
    if (meshLODCount > 0) {
        return lodMesh.GetLowestLOD();
    }

    return Resource::GeometryHandle::Invalid();
}

} // namespace DX12Engine::Renderer