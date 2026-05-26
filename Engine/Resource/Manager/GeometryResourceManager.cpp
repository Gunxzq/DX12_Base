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
// 几何体注册
// ============================================================================

GeometryHandle GeometryResourceManager::RegisterTriangleMesh(const TriangleMesh &mesh) {
    if (!m_initialized) {
        OutputDebugStringW(L"[ERROR] Not initialized!\n");
        return GeometryHandle::Invalid();
    }

    uint32_t index = AllocateEntry();
    if (index == UINT32_MAX) {
        return GeometryHandle::Invalid();
    }

    Entry &entry = m_entries[index];
    entry.mesh = mesh;
    entry.generation = m_nextGeneration++;
    entry.inUse = true;

    GeometryHandle handle;
    handle.index = index;
    handle.generation = entry.generation;

    char buf[256];
    sprintf_s(buf, "[INFO] Registered at index %d\n", index);
    OutputDebugStringA(buf);

    return handle;
}

// ============================================================================
// 几何体查询
// ============================================================================

const TriangleMesh *GeometryResourceManager::GetTriangleMesh(GeometryHandle handle) const {
    if (!IsValid(handle)) {
        return nullptr;
    }

    const Entry &entry = m_entries[handle.index];
    return &entry.mesh;
}

TriangleMesh *GeometryResourceManager::GetTriangleMesh(GeometryHandle handle) {
    if (!IsValid(handle)) {
        return nullptr;
    }

    Entry &entry = m_entries[handle.index];
    return &entry.mesh;
}

bool GeometryResourceManager::IsValid(GeometryHandle handle) const {
    if (!m_initialized) {
        return false;
    }

    if (handle.index >= m_capacity) {
        return false;
    }

    const Entry &entry = m_entries[handle.index];
    return entry.inUse && entry.generation == handle.generation;
}

const BoundingVolumeVariant *GeometryResourceManager::GetBounds(GeometryHandle handle) const {
    const TriangleMesh *mesh = GetTriangleMesh(handle);
    if (!mesh) {
        return nullptr;
    }
    return &mesh->localBounds;
}

// ============================================================================
// 几何体释放
// ============================================================================

void GeometryResourceManager::Release(GeometryHandle handle, uint64_t fenceValue) {
    if (!IsValid(handle)) {
        return;
    }

    // 标记为待释放
    PendingRelease pending;
    pending.index = handle.index;
    pending.generation = handle.generation;
    pending.fenceValue = fenceValue;
    m_pendingReleases.push_back(pending);

    // 标记条目为不再使用（但不清空数据，等 Reclaim 时释放）
    Entry &entry = m_entries[handle.index];
    entry.inUse = false;
    // 注意：不增加 generation，因为 Release 后可能还有旧的 handle 引用
    // 这些旧 handle 在 IsValid 时会因为 inUse=false 而失效
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
    if (!m_initialized) {
        return 0;
    }

    uint32_t count = 0;
    for (const auto &entry : m_entries) {
        if (entry.inUse) {
            ++count;
        }
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
    entry.mesh = TriangleMesh{}; // 重置为默认值
    entry.inUse = false;
    // generation 不重置，用于检测悬空句柄

    // 放回空闲列表
    m_freeList.push_back(index);
}

} // namespace DX12Engine::Resource