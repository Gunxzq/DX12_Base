#pragma once

// ========================================================================
// SpatialHashGrid — 空间哈希粗筛（原 OctreeSystem 改名，基于实际行为）
//
// 实际行为：均匀格子哈希（cellSize/halfCells/CellKey），非八叉树——
// 名称改为 SpatialHashGrid 消除误导（InstanceCullingSystem.md §6.1 改名表）。
// 对齐大型引擎主流：UE Spatial Hash / World Partition。
//
// 关键设计（2026-08-10 迁移自 OctreeSystem，原逻辑保留）：
//   - 实体入"其 worldBounds 覆盖的所有格子"（保守，防误删）
//   - 查询按"覆盖格子集合"遍历 + 实体级 FrustumCullSphere（radius×1.15，统一 GPU/CPU 剔除语义）
//   - 实体入多格 → 查询用 GTA 查询计数器去重（大型引擎 dedup，O(1) 替代 unordered_set）
//   - 双轨制：静态 Build（场景加载，O(N) 零扩容）+ 动态 AddEntity（运行时 spawn，扩容兜底）
//   - MarkDirty → PreCulling 阶段重建（与 RenderSlotCache::MarkDirty 同帧序）
// ========================================================================

#include "Math/BoundingVolume.h"
#include "Math/MathTypes.h"
#include "Renderer/Core/CulledSet.h"
#include "Renderer/Scene/Struct/Frustum.h"
#include <cstdint>
#include <unordered_map>
#include <vector>

namespace DX12Engine {
namespace Culling {

class SpatialHashGrid {
public:
    SpatialHashGrid() = default;
    ~SpatialHashGrid() = default;

    SpatialHashGrid(const SpatialHashGrid &) = delete;
    SpatialHashGrid &operator=(const SpatialHashGrid &) = delete;

    // ========================================================================
    // 生命周期
    // ========================================================================

    void Initialize(const DirectX::XMFLOAT3 &worldCenter, float worldSize);
    void Shutdown();
    bool IsInitialized() const { return m_initialized; }

    // ========================================================================
    // 构建与更新（双轨制）
    // ========================================================================

    /// 动态轨：运行时低频 spawn（扩容兜底；实体入其 worldBounds 覆盖的所有格子）
    void AddEntity(ECS::Entity entity, const Math::BoundingAABB &worldBounds, uint64_t sceneId = 0,
                   float cullDistance = 0.0f, bool forceVisible = false);

    /// 移除实体（遍历格子桶删除）
    void RemoveEntity(ECS::Entity entity);

    /// 清空所有实体（重建前调用）
    void Clear();

    /// 标记脏（实体增删改时调用；下帧查询前重建）
    void MarkDirty() { m_dirty = true; }
    bool IsDirty() const { return m_dirty; }
    void ClearDirty() { m_dirty = false; }

    /// 静态轨：批量构建（阶段 1 按全部实体包围盒预计算 worldCenter/worldSize 覆盖 + 1.2 余量、
    /// 封顶 65536；阶段 2 批量 InsertEntry，O(N) 零扩容）
    void Build(const std::vector<Renderer::CulledSet::Entry> &entities);

    /// Build 后补录剔除元数据（cullDistance @CullFar 拒远 / forceVisible 绕过剔除）
    void SetEntityCullData(const Renderer::CulledSet::Entry &entry, float cullDistance, bool forceVisible);

    // ========================================================================
    // 查询
    // ========================================================================

    /// 视锥查询：遍历视锥覆盖格子 → 格子内实体 FrustumCullSphere 粗筛 + cullDistance 拒远 → 候选集
    void QueryFrustum(const Renderer::Frustum &frustum, Renderer::CulledSet &outSet,
                      const DirectX::XMFLOAT3 &cameraPos) const;

    /// 分块视锥查询（并行剔除：Editor 注册 N 个并行 system 各查一块，合并 system 拼接去重）
    void QueryFrustumChunk(const Renderer::Frustum &frustum, Renderer::CulledSet &outSet,
                           const DirectX::XMFLOAT3 &cameraPos, uint32_t chunkIndex, uint32_t chunkCount) const;

    /// 上次视锥查询命中的格子数（性能观察：区块维度）
    uint32_t GetLastCellsHit() const { return m_lastCellsHit; }
    /// 空间索引当前格子数（诊断：确认空间索引是否已建立——0 = 从未 AddEntity）
    size_t GetCellCount() const { return m_cells.size(); }

    /// 包围盒查询：获取与给定 AABB 覆盖格子相交的所有实体（空间粗筛）
    void QueryBounds(const Math::BoundingAABB &bounds, std::vector<ECS::Entity> &outEntities) const;

    /// 射线查询：射线 AABB 覆盖格子（空间粗筛，不精确测试）
    void QueryRay(const FRay &ray,
                  std::vector<ECS::Entity> &outEntities) const; // FRay 在 DX12Engine 命名空间（MathTypes.h）

private:
    /// AABB 覆盖的格子坐标范围（NaN/Inf 防御 + clamp ±halfCells，2026-08-10 防天文循环）
    void CellRangeForBounds(const Math::BoundingAABB &b, int &minX, int &minY, int &minZ, int &maxX, int &maxY,
                            int &maxZ) const;
    /// 格子坐标 → 线性索引（偏移 m_halfCells 保证非负）
    int64_t CellKey(int cx, int cy, int cz) const;
    /// 单实体入格（覆盖其 worldBounds 的所有格子）——扩容重哈希/正常入格统一入口
    void InsertEntry(const Renderer::CulledSet::Entry &e);

    DirectX::XMFLOAT3 m_worldCenter = {0.0f, 0.0f, 0.0f};
    float m_worldSize = 0.0f;
    float m_cellSize = 250.0f; // 格子边长（City 2726 对角 → 约 11 格/轴）
    int m_halfCells = 6;       // 半轴格子数（worldSize/2/cellSize）
    bool m_initialized = false;
    bool m_dirty = true; // 初始脏（首次查询前必重建）；实体 CRUD 时 MarkDirty

    // 稀疏格子桶：key = 格子线性索引 → 该格子内实体（实体可入多格，保守防误删）
    std::unordered_map<int64_t, std::vector<Renderer::CulledSet::Entry>> m_cells;
    mutable uint32_t m_lastCellsHit = 0; // 上次视锥查询命中格子数（性能观察）
    // 实体 → 剔除距离（@CullFar；粗筛层拒远用）
    std::unordered_map<ECS::Entity, float> m_cullDistances;
    // 强制可见实体（BlockComponent.forceVisible——绕过剔除系统，查询时始终进入候选集）
    std::vector<Renderer::CulledSet::Entry> m_forceVisibleEntities;
    // GTA 查询计数器模式（大型引擎 dedup）：每次查询递增 stamp，实体存最近查询标记，
    // 比对决定是否跳过（O(1) 去重，替代每帧分配 unordered_set；实体入多格只处理一次）
    // 视锥/包围盒查询各自独立 stamp + map（避免同帧不同查询互相干扰）
    mutable uint32_t m_frustumStamp = 0;
    mutable std::unordered_map<ECS::Entity, uint32_t> m_queryStamps;
    mutable uint32_t m_boundsStamp = 0;
    mutable std::unordered_map<ECS::Entity, uint32_t> m_boundsStamps;
};

} // namespace Culling
} // namespace DX12Engine
