#include "OctreeSystem.h"
#include "Common/EngineAssert.h"
#include <algorithm>
#include <cmath>

namespace DX12Engine::Renderer {

// ========================================================================
// 生命周期
// ========================================================================

void OctreeSystem::Initialize(const DirectX::XMFLOAT3 &worldCenter, float worldSize) {
    Shutdown();
    m_root = std::make_unique<OctreeNode>(worldCenter, worldSize);
    m_worldSize = worldSize;
    m_maxDepth = 1;
    m_initialized = true;
}

void OctreeSystem::Shutdown() {
    m_root.reset();
    m_initialized = false;
    m_maxDepth = 0;
    m_worldSize = 0.0f;
}

// ========================================================================
// 构建与更新
// ========================================================================

void OctreeSystem::Clear() {
    if (m_root) {
        m_root = std::make_unique<OctreeNode>(m_root->center, m_root->size);
        m_maxDepth = 1;
    }
}

void OctreeSystem::Build(const std::vector<CulledSet::Entry> &entities) {
    Clear();
    if (!m_root)
        return;
    for (const auto &entry : entities) {
        Insert(m_root.get(), entry, 0);
    }
}

void OctreeSystem::AddEntity(ECS::Entity entity, const Math::BoundingAABB &worldBounds, uint64_t sceneId) {
    if (!m_root)
        return;
    CulledSet::Entry entry;
    entry.entity = entity;
    entry.worldBounds = worldBounds;
    entry.sceneId = sceneId;
    Insert(m_root.get(), entry, 0);
}

void OctreeSystem::RemoveEntity(ECS::Entity entity) {
    if (!m_root)
        return;
    // 简单实现：遍历所有节点移除匹配实体（低频调用，性能可接受）
    // 后续可优化：记录实体→节点映射
    auto removeFromNode = [&](auto &self, OctreeNode *node) -> void {
        if (!node)
            return;
        auto &entries = node->entries;
        entries.erase(std::remove_if(entries.begin(), entries.end(),
                                     [entity](const CulledSet::Entry &e) { return e.entity == entity; }),
                      entries.end());
        if (!node->IsLeaf()) {
            for (int i = 0; i < 8; ++i) {
                if (node->children[i])
                    self(self, node->children[i].get());
            }
        }
    };
    removeFromNode(removeFromNode, m_root.get());
}

// ========================================================================
// 八叉树节点操作
// ========================================================================

int OctreeSystem::GetChildIndex(const OctreeNode *node, const Math::BoundingAABB &bounds) const {
    DirectX::XMFLOAT3 center = node->center;
    DirectX::XMFLOAT3 bCenter = bounds.GetCenter();
    int index = 0;
    if (bCenter.x >= center.x) index |= 1;
    if (bCenter.y >= center.y) index |= 2;
    if (bCenter.z >= center.z) index |= 4;
    return index;
}

void OctreeSystem::OctreeNode::Split() {
    if (!IsLeaf())
        return;
    float halfSize = size * 0.5f;
    float quarterSize = size * 0.25f;
    for (int i = 0; i < 8; ++i) {
        DirectX::XMFLOAT3 childCenter;
        childCenter.x = center.x + ((i & 1) ? quarterSize : -quarterSize);
        childCenter.y = center.y + ((i & 2) ? quarterSize : -quarterSize);
        childCenter.z = center.z + ((i & 4) ? quarterSize : -quarterSize);
        children[i] = std::make_unique<OctreeNode>(childCenter, halfSize);
    }
    // 将当前节点的实体下放给子节点
    for (auto &entry : entries) {
        // 找到最适合的子节点
        int childIdx = 0;
        DirectX::XMFLOAT3 bCenter = entry.worldBounds.GetCenter();
        auto &c = center;
        if (bCenter.x >= c.x) childIdx |= 1;
        if (bCenter.y >= c.y) childIdx |= 2;
        if (bCenter.z >= c.z) childIdx |= 4;
        children[childIdx]->entries.push_back(entry);
    }
    entries.clear();
}

float OctreeSystem::OctreeNode::GetDistanceSqToBounds(const Math::BoundingAABB &bounds) const {
    // 计算节点中心到 AABB 最近点的距离平方
    DirectX::XMFLOAT3 bCenter = bounds.GetCenter();
    float dx = (std::max)(0.0f, std::abs(bCenter.x - center.x) - size * 0.5f);
    float dy = (std::max)(0.0f, std::abs(bCenter.y - center.y) - size * 0.5f);
    float dz = (std::max)(0.0f, std::abs(bCenter.z - center.z) - size * 0.5f);
    return dx * dx + dy * dy + dz * dz;
}

void OctreeSystem::Insert(OctreeNode *node, const CulledSet::Entry &entry, int depth) {
    if (!node)
        return;

    // 松散边界检查：使用 LOOSENESS 因子扩大节点范围
    float looseSize = node->size * LOOSENESS;
    DirectX::XMFLOAT3 bCenter = entry.worldBounds.GetCenter();
    float dx = std::abs(bCenter.x - node->center.x);
    float dy = std::abs(bCenter.y - node->center.y);
    float dz = std::abs(bCenter.z - node->center.z);
    float halfLoose = looseSize * 0.5f;

    // 如果实体完全在松散边界内且是叶子节点
    if (dx <= halfLoose && dy <= halfLoose && dz <= halfLoose) {
        if (node->IsLeaf()) {
            node->entries.push_back(entry);
            // 超过阈值且节点大小大于最小值 → 分裂
            if (node->entries.size() > MAX_ENTITIES_PER_NODE && node->size > MIN_NODE_SIZE) {
                node->Split();
            }
        } else {
            // 非叶子节点 → 放入合适的子节点
            int childIdx = GetChildIndex(node, entry.worldBounds);
            Insert(node->children[childIdx].get(), entry, depth + 1);
        }
    } else {
        // 实体跨越松散边界 → 留在当前节点
        node->entries.push_back(entry);
    }

    if (depth + 1 > static_cast<int>(m_maxDepth))
        m_maxDepth = depth + 1;
}

// ========================================================================
// 查询
// ========================================================================

void OctreeSystem::QueryFrustum(const Frustum &frustum, CulledSet &outSet) const {
    if (!m_root)
        return;
    outSet.Clear();
    QueryFrustumRecursive(m_root.get(), frustum, outSet);
}

void OctreeSystem::QueryFrustumRecursive(const OctreeNode *node, const Frustum &frustum, CulledSet &outSet) const {
    if (!node)
        return;

    // 收集当前节点的实体
    for (const auto &entry : node->entries) {
        outSet.Add(entry.entity, entry.worldBounds, entry.sceneId);
    }

    // 递归子节点（不做节点级视锥测试，精确剔除由 CullingSystem::Cull 完成）
    if (!node->IsLeaf()) {
        for (int i = 0; i < 8; ++i) {
            if (node->children[i])
                QueryFrustumRecursive(node->children[i].get(), frustum, outSet);
        }
    }
}

void OctreeSystem::QueryBounds(const Math::BoundingAABB &bounds, std::vector<ECS::Entity> &outEntities) const {
    if (!m_root)
        return;
    outEntities.clear();
    QueryBoundsRecursive(m_root.get(), bounds, outEntities);
}

void OctreeSystem::QueryBoundsRecursive(const OctreeNode *node, const Math::BoundingAABB &bounds,
                                         std::vector<ECS::Entity> &outEntities) const {
    if (!node)
        return;

    // 检查节点与查询 AABB 是否相交
    float half = node->size * 0.5f;
    float nMinX = node->center.x - half, nMaxX = node->center.x + half;
    float nMinY = node->center.y - half, nMaxY = node->center.y + half;
    float nMinZ = node->center.z - half, nMaxZ = node->center.z + half;

    if (nMaxX < bounds.min.x || nMinX > bounds.max.x ||
        nMaxY < bounds.min.y || nMinY > bounds.max.y ||
        nMaxZ < bounds.min.z || nMinZ > bounds.max.z)
        return;

    // 收集本节点实体
    for (const auto &entry : node->entries) {
        outEntities.push_back(entry.entity);
    }

    if (!node->IsLeaf()) {
        for (int i = 0; i < 8; ++i) {
            if (node->children[i])
                QueryBoundsRecursive(node->children[i].get(), bounds, outEntities);
        }
    }
}

void OctreeSystem::QueryRay(const FRay &ray, std::vector<ECS::Entity> &outEntities) const {
    if (!m_root)
        return;
    outEntities.clear();
    QueryRayRecursive(m_root.get(), ray, outEntities);
}

void OctreeSystem::QueryRayRecursive(const OctreeNode *node, const FRay &ray,
                                      std::vector<ECS::Entity> &outEntities) const {
    if (!node)
        return;

    // 测试射线与节点 AABB 相交
    float half = node->size * 0.5f;
    Math::BoundingAABB nodeBounds;
    nodeBounds.min = {node->center.x - half, node->center.y - half, node->center.z - half};
    nodeBounds.max = {node->center.x + half, node->center.y + half, node->center.z + half};

    // 简化射线-AABB 测试（使用已有的辅助函数）
    // 这里直接收集所有节点实体，由调用方做精确测试
    // 后续可以完善 IntersectAABB 函数
    for (const auto &entry : node->entries) {
        outEntities.push_back(entry.entity);
    }

    if (!node->IsLeaf()) {
        for (int i = 0; i < 8; ++i) {
            if (node->children[i])
                QueryRayRecursive(node->children[i].get(), ray, outEntities);
        }
    }
}

} // namespace DX12Engine::Renderer
