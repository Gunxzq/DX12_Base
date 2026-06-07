#include "DescriptorSlotAllocator.h"
#include <algorithm>
#include <cassert>

using namespace DX12Engine::Resource;

namespace DX12Engine::Resource {

/**
 * @brief 初始化描述符槽分配器
 * @param config 配置参数
 * @date 2026-05-24
 */
void DescriptorSlotAllocator::Initialize(const DescriptorSlotAllocatorConfig &config) {
    if (m_initialized)
        Shutdown();

    m_config = config;
    m_freeIndices.clear();
    m_pendingFree.clear();

    // BugFix: 必须在 Reserve 之前设置 m_initialized = true，
    // 否则 Reserve 的守卫条件会直接 return，导致 m_capacity 和 m_freeIndices 未初始化。
    m_initialized = true;
    Reserve(m_config.initialCapacity); // 初始化容量
}

/**
 * @brief 关闭描述符槽分配器
 * @date 2026-05-24
 */
void DescriptorSlotAllocator::Shutdown() {
    if (!m_initialized)
        return;

    m_freeIndices.clear();
    m_pendingFree.clear();
    m_capacity = 0;
    m_allocatedCount = 0;
    m_initialized = false;
}

/**
 * @brief 预留描述符槽容量
 * @param targetCapacity 目标容量
 * @date 2026-05-24
 */
void DescriptorSlotAllocator::Reserve(uint32_t targetCapacity) {
    if (!m_initialized)
        return;

    if (targetCapacity <= m_capacity)
        return;

    if (m_config.maxCapacity > 0 && targetCapacity > m_config.maxCapacity)
        targetCapacity = m_config.maxCapacity;

    if (targetCapacity <= m_capacity)
        return;

    // 扩展容量
    Expand(targetCapacity);
}

/**
 * @brief 扩展描述符槽容量
 * @param newCapacity 目标容量
 * @date 2026-05-24
 */
void DescriptorSlotAllocator::Expand(uint32_t newCapacity) {
    if (!HasFlag(m_config.flags, DescriptorSlotFlags::EnableExpand))
        return;

    if (m_config.maxCapacity > 0 && newCapacity > m_config.maxCapacity)
        newCapacity = m_config.maxCapacity;

    if (newCapacity <= m_capacity)
        return;

    // LinearAlloc 模式：不需要填充 m_freeIndices（始终从 m_nextIndex 分配）
    // FreeList 模式：将新索引推入 m_freeIndices 供后续复用
    if (!HasFlag(m_config.flags, DescriptorSlotFlags::LinearAlloc)) {
        for (uint32_t i = m_capacity; i < newCapacity; ++i) {
            m_freeIndices.push_back(i);
        }
    }

    m_capacity = newCapacity;
}

/**
 * @brief 分配描述符槽索引
 * @return uint32_t 索引值
 * @date 2026-05-24
 */
uint32_t DescriptorSlotAllocator::Allocate() {
    if (!m_initialized)
        return UINT32_MAX;

    bool linearAlloc = HasFlag(m_config.flags, DescriptorSlotFlags::LinearAlloc);

    if (linearAlloc) {
        return AllocateLinear();
    } else {
        return AllocateFromFreeList();
    }
}

/**
 * @brief 线性分配描述符槽索引
 * @return uint32_t 索引值
 * @date 2026-05-24
 */
uint32_t DescriptorSlotAllocator::AllocateLinear() {
    if (m_nextIndex >= m_capacity) {
        if (HasFlag(m_config.flags, DescriptorSlotFlags::EnableExpand)) {
            uint32_t newCapacity = m_capacity == 0 ? m_config.initialCapacity : m_capacity * 2;
            Expand(newCapacity);
        }

        if (m_nextIndex >= m_capacity)
            return UINT32_MAX;
    }

    uint32_t index = m_nextIndex;
    ++m_nextIndex;
    ++m_allocatedCount;
    return index;
}

/**
 * @brief 从可用索引列表中分配描述符槽索引
 * @return uint32_t 索引值
 * @date 2026-05-24
 */
uint32_t DescriptorSlotAllocator::AllocateFromFreeList() {
    if (!m_freeIndices.empty()) {
        uint32_t index = m_freeIndices.back();
        m_freeIndices.pop_back();
        ++m_allocatedCount;
        return index;
    }

    if (m_nextIndex < m_capacity) {
        uint32_t index = m_nextIndex;
        ++m_nextIndex;
        ++m_allocatedCount;
        return index;
    }

    if (HasFlag(m_config.flags, DescriptorSlotFlags::EnableExpand)) {
        uint32_t newCapacity = m_capacity == 0 ? m_config.initialCapacity : m_capacity * 2;
        Expand(newCapacity);

        // BugFix: 扩容后 m_freeIndices 已被填充，必须从 m_freeIndices 分配，
        // 不能走 m_nextIndex 路径，否则会和 m_freeIndices 中的索引冲突导致双重分配。
        if (!m_freeIndices.empty()) {
            uint32_t index = m_freeIndices.back();
            m_freeIndices.pop_back();
            ++m_allocatedCount;
            return index;
        }
    }

    return UINT32_MAX;
}

/**
 * @brief 分配 count 个连续描述符槽，返回起始索引
 * @param count 需要的连续槽位数量
 * @return uint32_t 起始索引，失败返回 UINT32_MAX
 * @note 仅 LinearAlloc 模式保证 O(1)；FreeList 模式会扫描空闲列表查找连续块
 * @date 2026-06-07
 */
uint32_t DescriptorSlotAllocator::AllocateConsecutive(uint32_t count) {
    if (!m_initialized || count == 0)
        return UINT32_MAX;

    bool linearAlloc = HasFlag(m_config.flags, DescriptorSlotFlags::LinearAlloc);

    if (linearAlloc) {
        // 线性分配：检查当前 m_nextIndex 开始的连续区域是否可用
        if (m_nextIndex + count > m_capacity) {
            if (HasFlag(m_config.flags, DescriptorSlotFlags::EnableExpand)) {
                uint32_t newCapacity = m_capacity == 0 ? m_config.initialCapacity : m_capacity * 2;
                while (newCapacity < m_nextIndex + count)
                    newCapacity *= 2;
                Expand(newCapacity);
            }
            if (m_nextIndex + count > m_capacity)
                return UINT32_MAX;
        }
        uint32_t baseIndex = m_nextIndex;
        m_nextIndex += count;
        m_allocatedCount += count;
        return baseIndex;
    }

    // FreeList 模式：扫描空闲列表查找连续块
    if (!m_freeIndices.empty()) {
        // 空闲列表是未排序的，先排序后扫描
        std::sort(m_freeIndices.begin(), m_freeIndices.end());

        uint32_t runStart = m_freeIndices[0];
        uint32_t runLen = 1;
        for (size_t i = 1; i < m_freeIndices.size(); ++i) {
            if (m_freeIndices[i] == m_freeIndices[i - 1] + 1) {
                runLen++;
                if (runLen >= count) {
                    // 找到连续块，从空闲列表中移除
                    uint32_t baseIndex = runStart;
                    // 移除这 count 个索引
                    auto it = std::lower_bound(m_freeIndices.begin(), m_freeIndices.end(), baseIndex);
                    m_freeIndices.erase(it, it + count);
                    m_allocatedCount += count;
                    return baseIndex;
                }
            } else {
                runStart = m_freeIndices[i];
                runLen = 1;
            }
        }
    }

    // 空闲列表中没找到连续块，尝试从 m_nextIndex 开始分配
    if (m_nextIndex + count <= m_capacity) {
        uint32_t baseIndex = m_nextIndex;
        m_nextIndex += count;
        m_allocatedCount += count;
        return baseIndex;
    }

    // 尝试扩容
    if (HasFlag(m_config.flags, DescriptorSlotFlags::EnableExpand)) {
        uint32_t newCapacity = m_capacity == 0 ? m_config.initialCapacity : m_capacity * 2;
        while (newCapacity < m_nextIndex + count)
            newCapacity *= 2;
        Expand(newCapacity);

        // BugFix: 扩容后 m_freeIndices 已被填充新索引，必须从 m_freeIndices 中扫描连续块，
        // 不能走 m_nextIndex 路径，否则会和 m_freeIndices 中的索引冲突导致双重分配。
        if (!m_freeIndices.empty()) {
            std::sort(m_freeIndices.begin(), m_freeIndices.end());

            uint32_t runStart = m_freeIndices[0];
            uint32_t runLen = 1;
            for (size_t i = 1; i < m_freeIndices.size(); ++i) {
                if (m_freeIndices[i] == m_freeIndices[i - 1] + 1) {
                    runLen++;
                    if (runLen >= count) {
                        uint32_t baseIndex = runStart;
                        auto it = std::lower_bound(m_freeIndices.begin(), m_freeIndices.end(), baseIndex);
                        m_freeIndices.erase(it, it + count);
                        m_allocatedCount += count;
                        return baseIndex;
                    }
                } else {
                    runStart = m_freeIndices[i];
                    runLen = 1;
                }
            }
        }

        // 扩容后 freeList 中也没有连续块，再走 m_nextIndex（此时 freeList 中不应有 m_nextIndex 范围的索引）
        if (m_nextIndex + count <= m_capacity) {
            uint32_t baseIndex = m_nextIndex;
            m_nextIndex += count;
            m_allocatedCount += count;
            return baseIndex;
        }
    }

    return UINT32_MAX;
}

/**
 * @brief 释放描述符槽索引
 * @param index 索引值
 * @param fenceValue 命令完成栏值
 * @date 2026-05-24
 */
void DescriptorSlotAllocator::Free(uint32_t index, uint64_t fenceValue) {
    if (!m_initialized || index >= m_capacity)
        return;

    if (HasFlag(m_config.flags, DescriptorSlotFlags::LinearAlloc))
        return;

    if (HasFlag(m_config.flags, DescriptorSlotFlags::DelayRelease)) {
        m_pendingFree.push_back({index, fenceValue});
    } else {
        m_freeIndices.push_back(index);
        --m_allocatedCount;
    }
}

/**
 * @brief 回收已释放的描述符槽索引
 * @param completedFence 已完成的命令栏值
 * @date 2026-05-24
 */
void DescriptorSlotAllocator::Reclaim(uint64_t completedFence) {
    if (!HasFlag(m_config.flags, DescriptorSlotFlags::DelayRelease))
        return;

    auto it = m_pendingFree.begin();
    while (it != m_pendingFree.end()) {
        if (completedFence >= it->fenceValue) {
            m_freeIndices.push_back(it->index);
            --m_allocatedCount;
            it = m_pendingFree.erase(it);
        } else {
            ++it;
        }
    }
}

/**
 * @brief 重置描述符槽分配器
 * @date 2026-05-24
 */
void DescriptorSlotAllocator::Reset() {
    if (!m_initialized)
        return;

    m_freeIndices.clear();
    m_pendingFree.clear();
    m_nextIndex = 0;
    m_allocatedCount = 0;

    for (uint32_t i = 0; i < m_capacity; ++i) {
        m_freeIndices.push_back(i);
    }
}

} // namespace DX12Engine::Resource