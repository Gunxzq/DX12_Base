#include "CommandAllocatorPool.h"

#include "Common/Common.h"

namespace DX12Engine::Renderer {

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
        if (entry && entry->inUse.load(std::memory_order_relaxed)) {
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

    size_t startIdx = m_nextIndex.fetch_add(1, std::memory_order_relaxed) % poolSize;

    for (size_t i = 0; i < poolSize; ++i) {
        size_t idx = (startIdx + i) % poolSize;
        auto &entry = m_pool[idx];

        bool expectedInUse = false;
        if (entry->inUse.compare_exchange_strong(expectedInUse, true, std::memory_order_acquire)) {

            uint64_t lastFence = entry->lastFenceValue.load(std::memory_order_acquire);

            if (lastFence <= currentGpuCompletedValue) {
                entry->allocator->Reset();
                return {idx, entry->allocator.get()};
            } else {
                entry->inUse.store(false, std::memory_order_release);
            }
        }
    }

    // 扩容
    {
        std::lock_guard<std::mutex> lock(m_expandMutex);
        if (m_pool.size() == poolSize) {
            Expand(poolSize * 2);
        }
    }

    return Acquire(currentGpuCompletedValue);
}

template <D3D12_COMMAND_LIST_TYPE Type>
void CommandAllocatorPool<Type>::Release(const Handle &handle, uint64_t fenceValue) {
    assert(handle.IsValid() && "Invalid Handle released");
    assert(handle.index < m_pool.size() && "Handle index out of bounds");

    auto &entry = m_pool[handle.index];
    entry->lastFenceValue.store(fenceValue, std::memory_order_release);
    entry->inUse.store(false, std::memory_order_release);
}

template <D3D12_COMMAND_LIST_TYPE Type> void CommandAllocatorPool<Type>::Expand(size_t newSize) {
    size_t oldSize = m_pool.size();
    for (size_t i = oldSize; i < newSize; ++i) {
        auto entry = std::make_unique<Entry>();
        entry->allocator = std::make_unique<CommandAllocator<Type>>(m_device);
        entry->lastFenceValue.store(0, std::memory_order_relaxed);
        entry->inUse.store(false, std::memory_order_relaxed);
        m_pool.push_back(std::move(entry));
    }
}

template class CommandAllocatorPool<D3D12_COMMAND_LIST_TYPE_DIRECT>;
template class CommandAllocatorPool<D3D12_COMMAND_LIST_TYPE_COMPUTE>;
template class CommandAllocatorPool<D3D12_COMMAND_LIST_TYPE_COPY>;

} // namespace DX12Engine::Renderer
