#include "Renderer/Core/Command/Allocator/CommandAllocatorPool.h"

namespace DX12Engine::Renderer {

// ========================================================================
// 模板成员函数实现
// ========================================================================

template <D3D12_COMMAND_LIST_TYPE Type>
CommandAllocatorPool<Type>::CommandAllocatorPool(ID3D12Device *device, size_t initialSize) : m_device(device) {
    Expand(initialSize);
}

template <D3D12_COMMAND_LIST_TYPE Type> CommandAllocatorPool<Type>::~CommandAllocatorPool() = default;

template <D3D12_COMMAND_LIST_TYPE Type> void CommandAllocatorPool<Type>::Shutdown() {
    std::lock_guard<std::mutex> lock(m_expandMutex);
    m_pool.clear();
}

template <D3D12_COMMAND_LIST_TYPE Type>
typename ICommandAllocatorPool::Stats CommandAllocatorPool<Type>::GetStats() const {
    typename ICommandAllocatorPool::Stats stats;
    stats.totalCount = m_pool.size();
    for (const auto &entry : m_pool) {
        if (entry.inUse.load(std::memory_order_relaxed)) {
            ++stats.inUseCount;
        }
    }
    return stats;
}

template <D3D12_COMMAND_LIST_TYPE Type>
typename CommandAllocatorPool<Type>::Handle CommandAllocatorPool<Type>::Acquire(uint64_t currentGpuCompletedValue) {
    size_t poolSize = m_pool.size();
    if (poolSize == 0) {
        return {static_cast<size_t>(-1), nullptr};
    }

    // 使用原子递增实现简单的轮询起点，分散竞争
    size_t startIdx = m_nextIndex.fetch_add(1, std::memory_order_relaxed) % poolSize;

    for (size_t i = 0; i < poolSize; ++i) {
        size_t idx = (startIdx + i) % poolSize;
        Entry &entry = m_pool[idx];

        // 1. CAS 尝试获取独占权
        bool expectedInUse = false;
        if (entry.inUse.compare_exchange_strong(expectedInUse, true, std::memory_order_acquire)) {

            // 2. 检查 GPU 是否已完成 (读取原子变量)
            uint64_t lastFence = entry.lastFenceValue.load(std::memory_order_acquire);

            if (lastFence <= currentGpuCompletedValue) {
                // 3. 安全：重置并返回
                entry.allocator->Reset();
                return {idx, entry.allocator.get()};
            } else {
                // 4. 不安全：GPU 仍在运行，释放独占权，继续寻找
                entry.inUse.store(false, std::memory_order_release);
            }
        }
    }

    // 5. 所有分配器都不可用，尝试扩容
    {
        std::lock_guard<std::mutex> lock(m_expandMutex);
        // 双重检查，避免多线程重复扩容
        if (m_pool.size() == poolSize) {
            Expand(poolSize * 2);
        }
    }

    // 重试一次（新分配的 Allocator 肯定可用）
    return Acquire(currentGpuCompletedValue);
}

template <D3D12_COMMAND_LIST_TYPE Type>
void CommandAllocatorPool<Type>::Release(const Handle &handle, uint64_t fenceValue) {
    assert(handle.IsValid() && "Invalid Handle released");
    assert(handle.index < m_pool.size() && "Handle index out of bounds");

    Entry &entry = m_pool[handle.index];

    // 更新 Fence 值 (原子写入)
    entry.lastFenceValue.store(fenceValue, std::memory_order_release);

    // 标记为未使用 (原子写入，允许其他线程再次 Acquire)
    entry.inUse.store(false, std::memory_order_release);
}

template <D3D12_COMMAND_LIST_TYPE Type> void CommandAllocatorPool<Type>::Expand(size_t newSize) {
    size_t oldSize = m_pool.size();
    m_pool.resize(newSize);

    for (size_t i = oldSize; i < newSize; ++i) {
        m_pool[i].allocator = std::make_unique<CommandAllocator<Type>>(m_device);
        m_pool[i].lastFenceValue.store(0, std::memory_order_relaxed);
        m_pool[i].inUse.store(false, std::memory_order_relaxed);
    }
}

// ========================================================================
// 显式实例化定义
// 强制编译器为这三种类型生成代码
// ========================================================================

template class CommandAllocatorPool<D3D12_COMMAND_LIST_TYPE_DIRECT>;
template class CommandAllocatorPool<D3D12_COMMAND_LIST_TYPE_COMPUTE>;
template class CommandAllocatorPool<D3D12_COMMAND_LIST_TYPE_COPY>;

} // namespace DX12Engine::Renderer