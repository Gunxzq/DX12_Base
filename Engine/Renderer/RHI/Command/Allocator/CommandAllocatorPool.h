#pragma once

#include "CommandAllocator.h"
#include <atomic>
#include <deque>
#include <memory>
#include <mutex>
#include <vector>

namespace DX12Engine::Renderer {

class ICommandAllocatorPool {
public:
    virtual ~ICommandAllocatorPool() = default;
    virtual void Shutdown() = 0;

    struct Stats {
        size_t totalCount = 0;
        size_t inUseCount = 0;
    };
    virtual Stats GetStats() const = 0;
};

template <D3D12_COMMAND_LIST_TYPE Type> class CommandAllocatorPool : public ICommandAllocatorPool {
public:
    struct Handle {
        size_t index = static_cast<size_t>(-1);
        CommandAllocator<Type> *allocator = nullptr;

        bool IsValid() const { return index != static_cast<size_t>(-1); }
    };

    explicit CommandAllocatorPool(ID3D12Device *device, size_t initialSize = 8);
    ~CommandAllocatorPool() override;

    CommandAllocatorPool(const CommandAllocatorPool &) = delete;
    CommandAllocatorPool &operator=(const CommandAllocatorPool &) = delete;

    void Shutdown() override;
    typename ICommandAllocatorPool::Stats GetStats() const override;

    Handle Acquire(uint64_t currentGpuCompletedValue);
    void Release(const Handle &handle, uint64_t fenceValue);

private:
    struct alignas(64) Entry {
        std::unique_ptr<CommandAllocator<Type>> allocator;
        std::atomic<uint64_t> lastFenceValue{0};
        std::atomic<bool> inUse{false};
    };

    // 使用 deque<unique_ptr<Entry>>：Entry 堆上独立分配，push_back 永不失效已有引用
    std::deque<std::unique_ptr<Entry>> m_pool;
    std::atomic<size_t> m_nextIndex{0};

    ID3D12Device *m_device = nullptr;
    std::mutex m_expandMutex;

    void Expand(size_t newSize);
};

extern template class CommandAllocatorPool<D3D12_COMMAND_LIST_TYPE_DIRECT>;
extern template class CommandAllocatorPool<D3D12_COMMAND_LIST_TYPE_COMPUTE>;
extern template class CommandAllocatorPool<D3D12_COMMAND_LIST_TYPE_COPY>;

} // namespace DX12Engine::Renderer
