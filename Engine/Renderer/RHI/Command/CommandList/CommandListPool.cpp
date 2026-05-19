#include "CommandListPool.h"

namespace DX12Engine::Renderer {

// ========================================================================
// 模板成员函数实现
// ========================================================================

template <D3D12_COMMAND_LIST_TYPE Type>
CommandListPool<Type>::CommandListPool(ID3D12Device *device) : m_device(device), m_type(Type) {}

template <D3D12_COMMAND_LIST_TYPE Type> CommandListPool<Type>::~CommandListPool() = default;

template <D3D12_COMMAND_LIST_TYPE Type> void CommandListPool<Type>::Shutdown() {
    std::lock_guard<std::mutex> lock(m_expandMutex);
    m_pool.clear();
}

template <D3D12_COMMAND_LIST_TYPE Type> typename ICommandListPool::Stats CommandListPool<Type>::GetStats() const {
    typename ICommandListPool::Stats stats;
    stats.totalCount = m_pool.size();
    for (const auto &entry : m_pool) {
        if (entry.inUse.load(std::memory_order_relaxed)) {
            ++stats.inUseCount;
        }
    }
    return stats;
}

template <D3D12_COMMAND_LIST_TYPE Type>
typename CommandListPool<Type>::Handle CommandListPool<Type>::AcquireHandle(ID3D12CommandAllocator *allocator) {
    assert(allocator != nullptr);

    size_t poolSize = m_pool.size();
    if (poolSize > 0) {
        size_t startIdx = m_nextIndex.fetch_add(1, std::memory_order_relaxed) % poolSize;

        for (size_t i = 0; i < poolSize; ++i) {
            size_t idx = (startIdx + i) % poolSize;
            Entry &entry = m_pool[idx];

            bool expectedInUse = false;
            if (!entry.inUse.load(std::memory_order_relaxed)) {
                if (entry.inUse.compare_exchange_strong(expectedInUse, true, std::memory_order_acquire)) {
                    CommandList cmdList(entry.cmdList.Get());
                    PrepareCommandList(cmdList, allocator, entry);
                    return {idx};
                }
            }
        }
    }

    // 2. 慢速路径：没有空闲项，需要扩容（加锁）
    std::lock_guard<std::mutex> lock(m_expandMutex);

    // 双重检查：防止其他线程在等待锁期间已经创建了新的项或释放了旧的项
    for (size_t i = 0; i < m_pool.size(); ++i) {
        Entry &entry = m_pool[i];
        if (!entry.inUse.load(std::memory_order_relaxed)) {
            bool expectedInUse = false;
            if (entry.inUse.compare_exchange_strong(expectedInUse, true, std::memory_order_acquire)) {
                CommandList cmdList(entry.cmdList.Get());
                PrepareCommandList(cmdList, allocator, entry);
                return {i};
            }
        }
    }

    // 3. 真正需要创建新项
    size_t newIndex = m_pool.size();
    m_pool.emplace_back();

    HRESULT hr = m_device->CreateCommandList(0, m_type, allocator, nullptr, IID_PPV_ARGS(&m_pool[newIndex].cmdList));
    if (FAILED(hr)) {
        m_pool.pop_back(); // 回滚
        throw std::runtime_error("Failed to create CommandList in Pool");
    }
    // 设置初始状态
    m_pool[newIndex].inUse.store(true, std::memory_order_relaxed);
    m_pool[newIndex].needsClose.store(false, std::memory_order_release);

    return {newIndex};
}

template <D3D12_COMMAND_LIST_TYPE Type> CommandList CommandListPool<Type>::GetCommandList(const Handle &handle) {
    assert(handle.IsValid());
    assert(handle.index < m_pool.size());

    return CommandList(m_pool[handle.index].cmdList.Get());
}

// ========================================================================
// 私有辅助方法：准备 CommandList 状态
// ========================================================================

template <D3D12_COMMAND_LIST_TYPE Type>
void CommandListPool<Type>::PrepareCommandList(CommandList &cmdList, ID3D12CommandAllocator *allocator, Entry &entry) {
    bool expectedNeedsClose = true;
    if (entry.needsClose.compare_exchange_strong(expectedNeedsClose, false, std::memory_order_acquire)) {
        cmdList.Close();
    }

    // 此时 CommandList 一定是 Closed 状态，可以安全地 Reset
    cmdList.Reset(allocator, nullptr);

    // 标记为使用后需要 Close
    entry.needsClose.store(true, std::memory_order_release);
}

template <D3D12_COMMAND_LIST_TYPE Type> void CommandListPool<Type>::Release(const Handle &handle) {
    assert(handle.IsValid());
    assert(handle.index < m_pool.size());

    Entry &entry = m_pool[handle.index];

    assert(entry.inUse.load() == true);

    entry.inUse.store(false, std::memory_order_release);
    entry.needsClose.store(false, std::memory_order_release);
}

// ========================================================================
// 显式实例化定义
// ========================================================================

template class CommandListPool<D3D12_COMMAND_LIST_TYPE_DIRECT>;
template class CommandListPool<D3D12_COMMAND_LIST_TYPE_COMPUTE>;
template class CommandListPool<D3D12_COMMAND_LIST_TYPE_COPY>;

} // namespace DX12Engine::Renderer