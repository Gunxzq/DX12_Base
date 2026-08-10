#include "GeometryResourceManager.h"
#include "Common/Common.h"
#include <algorithm>
#include <cassert>

namespace DX12Engine::Resource {

void GeometryResourceManager::Initialize(uint32_t initialCapacity) {
    if (m_initialized) {
        Shutdown();
    }

    m_capacity = std::min(initialCapacity, MAX_CAPACITY);
    m_entries.resize(m_capacity);
    m_freeList.reserve(m_capacity);

    // 初始化空闲列表
    for (uint32_t i = 0; i < m_capacity; ++i) {
        m_freeList.push_back(m_capacity - 1 - i); // 倒序，便于从末尾取
    }

    m_nextGeneration = 1;
    m_initialized = true;
}

void GeometryResourceManager::Shutdown() {
    if (!m_initialized) {
        return;
    }

    // 注意：这里不释放 GPU 资源
    // GPU 资源由 GpuResourceManager 管理，需要调用者自行处理
    m_entries.clear();
    m_freeList.clear();
    m_pendingReleases.clear();
    m_capacity = 0;
    m_nextGeneration = 1;
    m_initialized = false;
}

// ============================================================================
// 统一注册
// ============================================================================

GeometryHandle GeometryResourceManager::RegisterGeometryVariant(const GeometryVariant &geometry) {
    if (!m_initialized) {
        ErrorReporter::Report("GeometryResourceManager: Not initialized");
        return GeometryHandle::Invalid();
    }

    uint32_t index = AllocateEntry();
    if (index == UINT32_MAX) {
        return GeometryHandle::Invalid();
    }

    Entry &entry = m_entries[index];
    entry.geometry = geometry;
    entry.generation = m_nextGeneration++;
    entry.refCount = 1; // 首次注册引用计数为 1
    entry.inUse = true;

    // 统一语义兜底：所有图元无子网格表时视为 1 个子网格（整个索引区间）
    // 消费方（Builder 等）不再判空（见 SubMeshMaterialSlots.md §2.3）
    auto fillSubMesh = [](GeometryBase &base) {
        if (base.subMeshes.empty()) {
            SubMeshInfo whole;
            whole.startIndex = 0;
            whole.indexCount = base.indexCount;
            whole.startVertex = 0; // 索引已绝对化，BaseVertexLocation 恒 0
            base.subMeshes.push_back(whole);
        }
    };
    if (auto *mesh = std::get_if<TriangleMesh>(&entry.geometry)) {
        fillSubMesh(*mesh);
    } else if (auto *grid = std::get_if<GridGeometry>(&entry.geometry)) {
        fillSubMesh(*grid);
    } else if (auto *patch = std::get_if<PatchMesh>(&entry.geometry)) {
        fillSubMesh(*patch);
    }

    GeometryHandle handle;
    handle.index = index;
    // BugFix: handle.generation 是 10 位字段，需要取模避免截断导致 IsValid 失败
    handle.generation = entry.generation & 0x3FF; // 低 10 位

    return handle;
}

// ============================================================================
// 查询
// ============================================================================

const GeometryVariant *GeometryResourceManager::GetGeometryVariant(GeometryHandle handle) const {
    if (!IsValid(handle))
        return nullptr;
    return &m_entries[handle.index].geometry;
}

GeometryVariant *GeometryResourceManager::GetGeometryVariant(GeometryHandle handle) {
    if (!IsValid(handle))
        return nullptr;
    return &m_entries[handle.index].geometry;
}

const GeometryBase *GeometryResourceManager::GetGeometryBase(GeometryHandle handle) const {
    const auto *variant = GetGeometryVariant(handle);
    if (!variant)
        return nullptr;
    return std::visit([](const auto &geom) -> const GeometryBase * { return &geom; }, *variant);
}

GeometryBase *GeometryResourceManager::GetGeometryBase(GeometryHandle handle) {
    auto *variant = GetGeometryVariant(handle);
    if (!variant)
        return nullptr;
    return std::visit([](auto &geom) -> GeometryBase * { return &geom; }, *variant);
}

size_t GeometryResourceManager::GetGeometryTypeIndex(GeometryHandle handle) const {
    const auto *variant = GetGeometryVariant(handle);
    if (!variant)
        return static_cast<size_t>(-1);
    return variant->index();
}

const char *GeometryResourceManager::GetGeometryTypeName(GeometryHandle handle) const {
    size_t idx = GetGeometryTypeIndex(handle);
    switch (idx) {
    case 0:
        return "TriangleMesh";
    case 1:
        return "PatchMesh";
    case 2:
        return "GridGeometry";
    default:
        return "Unknown";
    }
}

bool GeometryResourceManager::IsValid(GeometryHandle handle) const {
    if (!m_initialized) {
        return false;
    }

    if (handle.index >= m_capacity) {
        return false;
    }

    const Entry &entry = m_entries[handle.index];
    // BugFix: handle.generation 是 10 位，取低 10 位比较
    return entry.inUse && (entry.generation & 0x3FF) == (handle.generation & 0x3FF);
}

const Math::BoundingVolumeVariant *GeometryResourceManager::GetBounds(GeometryHandle handle) const {
    const auto *variant = GetGeometryVariant(handle);
    if (!variant)
        return nullptr;

    // 根据类型获取包围盒
    return std::visit([](const auto &geom) -> const Math::BoundingVolumeVariant * { return &geom.localBounds; },
                      *variant);
}

const std::vector<SubMeshInfo> *GeometryResourceManager::GetSubMeshInfo(GeometryHandle handle) const {
    const auto *base = GetGeometryBase(handle);
    if (!base)
        return nullptr;
    return &base->subMeshes;
}

// ============================================================================
// 释放与引用计数
// ============================================================================

void GeometryResourceManager::Retain(GeometryHandle handle) {
    if (!IsValid(handle)) {
        return;
    }
    m_entries[handle.index].refCount++;
}

void GeometryResourceManager::Release(GeometryHandle handle, uint64_t fenceValue) {
    if (!IsValid(handle)) {
        return;
    }

    Entry &entry = m_entries[handle.index];
    if (entry.refCount > 0) {
        entry.refCount--;
    }

    // 仍有引用，不释放
    if (entry.refCount > 0) {
        return;
    }

    // 标记为待释放
    PendingRelease pending;
    pending.index = handle.index;
    pending.generation = handle.generation;
    pending.fenceValue = fenceValue;
    m_pendingReleases.push_back(pending);

    entry.inUse = false;
}

void GeometryResourceManager::Reclaim(uint64_t completedFence) {
    auto it = m_pendingReleases.begin();
    while (it != m_pendingReleases.end()) {
        if (completedFence >= it->fenceValue) {
            // GPU 已经完成，可以安全回收条目
            FreeEntry(it->index);
            it = m_pendingReleases.erase(it);
        } else {
            ++it;
        }
    }
}

// ============================================================================
// 调试/统计
// ============================================================================

uint32_t GeometryResourceManager::GetActiveCount() const {
    if (!m_initialized)
        return 0;

    uint32_t count = 0;
    for (const auto &entry : m_entries) {
        if (entry.inUse)
            ++count;
    }
    return count;
}

uint32_t GeometryResourceManager::GetCapacity() const { return m_capacity; }

uint32_t GeometryResourceManager::GetPendingReleaseCount() const {
    return static_cast<uint32_t>(m_pendingReleases.size());
}

// ============================================================================
// 内部方法
// ============================================================================

uint32_t GeometryResourceManager::AllocateEntry() {
    if (m_freeList.empty()) {
        // 尝试扩展容量
        if (m_capacity >= MAX_CAPACITY) {
            return UINT32_MAX;
        }

        uint32_t newCapacity = std::min(m_capacity * 2, MAX_CAPACITY);
        if (newCapacity <= m_capacity) {
            return UINT32_MAX;
        }

        // 保存当前容量
        uint32_t oldCapacity = m_capacity;

        // 扩展数组
        m_entries.resize(newCapacity);

        // 添加新的空闲索引
        for (uint32_t i = newCapacity - 1; i >= oldCapacity; --i) {
            m_freeList.push_back(i);
        }

        m_capacity = newCapacity;
    }

    if (m_freeList.empty()) {
        return UINT32_MAX;
    }

    uint32_t index = m_freeList.back();
    m_freeList.pop_back();
    return index;
}

void GeometryResourceManager::FreeEntry(uint32_t index) {
    if (index >= m_capacity) {
        return;
    }

    // 清空数据
    Entry &entry = m_entries[index];
    entry.geometry = GeometryVariant{}; // 重置为默认值（空 variant）
    entry.inUse = false;
    // generation 不重置，用于检测悬空句柄

    // 放回空闲列表
    m_freeList.push_back(index);
}

} // namespace DX12Engine::Resource