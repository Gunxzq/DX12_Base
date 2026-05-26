#pragma once

#include "ECS/Core/Components.h"
#include "ECS/Core/Registry.h"
#include "Math/BoundingVolume.h"
#include "Renderer/Scene/Struct/Frustum.h"
#include <DirectXMath.h>
#include <unordered_map>

namespace DX12Engine {

namespace Math {
using BoundingVolumeVariant = Math::BoundingVolumeVariant;
}

namespace Renderer {

// ============================================================================
// 剔除结果
// ============================================================================
struct CullingResult {
    std::unordered_map<ECS::Entity, bool> visibleMap;

    void Clear() { visibleMap.clear(); }
    void SetVisible(ECS::Entity entity, bool visible) { visibleMap[entity] = visible; }
    bool IsVisible(ECS::Entity entity) const {
        auto it = visibleMap.find(entity);
        return it != visibleMap.end() && it->second;
    }
    size_t Size() const { return visibleMap.size(); }
};

// ============================================================================
// 剔除系统 - 负责计算实体的可见性
// ============================================================================
class CullingSystem {
public:
    CullingSystem() = default;
    ~CullingSystem() = default;

    // 禁止拷贝
    CullingSystem(const CullingSystem &) = delete;
    CullingSystem &operator=(const CullingSystem &) = delete;

    void SetFrustum(const Frustum *frustum) { m_frustum = frustum; }
    void Execute(ECS::Registry &registry, CullingResult &outResult);

private:
    bool Intersects(const Frustum &frustum, const Math::BoundingAABB &bounds) const;
    bool Intersects(const Frustum &frustum, const Math::BoundingSphere &bounds) const;
    bool TestVisibility(const Frustum &frustum, const Math::BoundingVolumeVariant &bounds,
                        const DirectX::XMFLOAT3 &worldPosition) const;

    const Frustum *m_frustum = nullptr;
};

} // namespace Renderer

} // namespace DX12Engine