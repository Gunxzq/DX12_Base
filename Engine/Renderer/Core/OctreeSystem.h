#pragma once

#include "CulledSet.h"
#include "ECS/Core/Entity.h"
#include "Math/BoundingVolume.h"
#include "Math/MathTypes.h"
#include "Renderer/Scene/Struct/Frustum.h"
#include <DirectXMath.h>
#include <memory>
#include <vector>

namespace DX12Engine::Renderer {

// ============================================================================
// OctreeSystem — 八叉树空间划分系统
//
// 定位：PreCulling 阶段运行，对全场景/世界实体做空间离散化
// 输出：视锥查询结果写入 CulledSet，供 PostCulling（CullingSystem）精筛
//
// 使用方式：
//   1. 每帧在 PreCulling 阶段 BuildOrUpdate(bounds, ...) 更新八叉树
//   2. QueryFrustum(frustum, outCulledSet) 粗筛候选实体
// ============================================================================
class OctreeSystem {
public:
    OctreeSystem() = default;
    ~OctreeSystem() = default;

    OctreeSystem(const OctreeSystem &) = delete;
    OctreeSystem &operator=(const OctreeSystem &) = delete;

    /// 八叉树节点最小尺寸（防止无限细分）
    static constexpr float MIN_NODE_SIZE = 2.0f;

    /// 每个节点允许的最大实体数（超过则分裂）
    static constexpr uint32_t MAX_ENTITIES_PER_NODE = 16;

    /// 松散度因子（1.0=标准八叉树，>1.0=松散）
    static constexpr float LOOSENESS = 1.25f;

    // ========================================================================
    // 生命周期
    // ========================================================================

    void Initialize(const DirectX::XMFLOAT3 &worldCenter, float worldSize);
    void Shutdown();
    bool IsInitialized() const { return m_initialized; }

    // ========================================================================
    // 构建与更新
    // ========================================================================

    /// 添加一个实体到八叉树（首次构建/增量添加）
    void AddEntity(ECS::Entity entity, const Math::BoundingAABB &worldBounds, uint64_t sceneId = 0);

    /// 移除实体
    void RemoveEntity(ECS::Entity entity);

    /// 清空所有实体（重建前调用）
    void Clear();

    /// 构建或更新八叉树（从外部实体列表重建）
    /// @param entities 实体列表（含包围盒和场景ID）
    void Build(const std::vector<CulledSet::Entry> &entities);

    // ========================================================================
    // 查询
    // ========================================================================

    /// 视锥查询：将八叉树中与视锥相交的实体写入 CulledSet
    /// @param frustum 视锥体（来自 CullingSystem）
    /// @param outSet  输出候选集（八叉树粗筛结果）
    void QueryFrustum(const Frustum &frustum, CulledSet &outSet) const;

    /// 包围盒查询：获取与给定 AABB 相交的所有实体
    void QueryBounds(const Math::BoundingAABB &bounds, std::vector<ECS::Entity> &outEntities) const;

    /// 射线查询：获取与射线相交的所有实体（仅空间粗筛，不精确测试）
    void QueryRay(const FRay &ray, std::vector<ECS::Entity> &outEntities) const;

private:
    // ========================================================================
    // 八叉树节点
    // ========================================================================
    struct OctreeNode {
        DirectX::XMFLOAT3 center;      // 节点中心
        float size;                    // 节点边长
        std::unique_ptr<OctreeNode> children[8]; // 子节点（nullptr = 叶子）
        std::vector<CulledSet::Entry> entries;   // 本节点实体

        OctreeNode(const DirectX::XMFLOAT3 &c, float s) : center(c), size(s) {}

        bool IsLeaf() const { return !children[0]; }

        /// 获取当前节点中心到给定 AABB 最近点的距离平方
        float GetDistanceSqToBounds(const Math::BoundingAABB &bounds) const;

        /// 分裂节点为 8 个子节点
        void Split();
    };

    // ========================================================================
    // 内部方法
    // ========================================================================

    /// 递归插入实体到合适的节点
    void Insert(OctreeNode *node, const CulledSet::Entry &entry, int depth);

    /// 递归视锥查询
    void QueryFrustumRecursive(const OctreeNode *node, const Frustum &frustum, CulledSet &outSet) const;

    /// 递归包围盒查询
    void QueryBoundsRecursive(const OctreeNode *node, const Math::BoundingAABB &bounds,
                              std::vector<ECS::Entity> &outEntities) const;

    /// 递归射线查询
    void QueryRayRecursive(const OctreeNode *node, const FRay &ray, std::vector<ECS::Entity> &outEntities) const;

    /// 获取实体所属的子节点索引（0-7）
    int GetChildIndex(const OctreeNode *node, const Math::BoundingAABB &bounds) const;

    // ========================================================================
    // 成员变量
    // ========================================================================

    std::unique_ptr<OctreeNode> m_root;
    bool m_initialized = false;
    uint32_t m_maxDepth = 0; // 当前最大深度
    float m_worldSize = 0.0f;
};

} // namespace DX12Engine::Renderer
