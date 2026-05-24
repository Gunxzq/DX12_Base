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

    Reserve(m_config.initialCapacity); // 初始化容量
    m_initialized = true;
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

    for (uint32_t i = m_capacity; i < newCapacity; ++i) {
        m_freeIndices.push_back(i);
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

        if (m_nextIndex < m_capacity) {
            uint32_t index = m_nextIndex;
            ++m_nextIndex;
            ++m_allocatedCount;
            return index;
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