#pragma once

#include "ECS/Core/Entity.h"
#include "Math/BoundingVolume.h"
#include <cstdint>
#include <vector>

namespace DX12Engine::Renderer {

// ============================================================================
// CulledSet — 剔除系统输出的可见集
//
// 定位：PreCulling（八叉树粗筛）→ PostCulling（视锥精筛）→ CulledSet
// 消费者：Builder 层（Opaque/Skinned/Terrain/Water 等）和 VisibleRaycaster
//
// 职责：
//   1. 携带经过视锥剔除的实体列表
//   2. 携带场景 ID（Editor 端用于按 Tab 过滤，Game 端统一为 0）
//   3. 只读，多 Builder 可并行消费
// ============================================================================
struct CulledSet {
    /// 可见实体条目：包含实体句柄和其世界空间包围盒
    struct Entry {
        ECS::Entity entity = ECS::INVALID_ENTITY;
        Math::BoundingAABB worldBounds; // 世界空间包围盒（已做视锥测试）
        uint64_t sceneId = 0;           // 场景 ID（Editor: sceneId, Game: 0）
    };

    std::vector<Entry> entries; // 可见实体列表

    bool IsEmpty() const { return entries.empty(); }
    size_t Size() const { return entries.size(); }

    void Clear() { entries.clear(); }
    void Reserve(size_t capacity) { entries.reserve(capacity); }

    /// 添加一个可见实体
    void Add(ECS::Entity entity, const Math::BoundingAABB &bounds, uint64_t sceneId = 0) {
        entries.push_back({entity, bounds, sceneId});
    }

    /// 按 sceneId 过滤出可见实体列表（Editor 端使用）
    /// @return 过滤后的实体列表，不包含包围盒信息
    std::vector<ECS::Entity> GetEntitiesByScene(uint64_t sceneId) const {
        std::vector<ECS::Entity> result;
        result.reserve(entries.size());
        for (auto &entry : entries) {
            if (entry.sceneId == sceneId)
                result.push_back(entry.entity);
        }
        return result;
    }

    /// 获取所有可见实体的完整条目
    const std::vector<Entry> &GetAll() const { return entries; }
};

} // namespace DX12Engine::Renderer
