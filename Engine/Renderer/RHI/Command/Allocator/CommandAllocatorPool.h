#pragma once

#include "CommandAllocator.h"
#include <mutex>
#include <vector>

namespace DX12Engine::Renderer {

// ========================================================================
// 1. 非模板基类接口
// ========================================================================

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

// ========================================================================
// 2. 模板类声明
// ========================================================================

/**
 * @brief 线程安全的无锁命令分配器池
 * @tparam Type 命令列表类型
 */
template <D3D12_COMMAND_LIST_TYPE Type> class CommandAllocatorPool : public ICommandAllocatorPool {
public:
    // 句柄：用于快速 Release，避免遍历查找
    struct Handle {
        size_t index = static_cast<size_t>(-1);
        CommandAllocator<Type> *allocator = nullptr;

        bool IsValid() const { return index != static_cast<size_t>(-1); }
    };

    explicit CommandAllocatorPool(ID3D12Device *device, size_t initialSize = 8);
    ~CommandAllocatorPool() override;

    CommandAllocatorPool(const CommandAllocatorPool &) = delete;
    CommandAllocatorPool &operator=(const CommandAllocatorPool &) = delete;

    // 实现基类接口
    void Shutdown() override;
    typename ICommandAllocatorPool::Stats GetStats() const override;

    // 工作线程接口
    Handle Acquire(uint64_t currentGpuCompletedValue);
    void Release(const Handle &handle, uint64_t fenceValue);

private:
    // 缓存行大小，防止伪共享
    static constexpr size_t CACHE_LINE_SIZE = 64;

    struct alignas(CACHE_LINE_SIZE) Entry {
        std::unique_ptr<CommandAllocator<Type>> allocator;
        std::atomic<uint64_t> lastFenceValue{0};
        std::atomic<bool> inUse{false};

        char padding[CACHE_LINE_SIZE - sizeof(std::unique_ptr<CommandAllocator<Type>>) - sizeof(std::atomic<uint64_t>) -
                     sizeof(std::atomic<bool>)];

        Entry() = default;

        Entry(const Entry &) = delete;
        Entry &operator=(const Entry &) = delete;

        Entry(Entry &&other) noexcept
            : allocator(std::move(other.allocator)), lastFenceValue(other.lastFenceValue.load()),
              inUse(other.inUse.load()) {
            other.inUse.store(false);
        }

        Entry &operator=(Entry &&other) noexcept {
            if (this != &other) {
                allocator = std::move(other.allocator);
                lastFenceValue.store(other.lastFenceValue.load());
                inUse.store(other.inUse.load());
                other.inUse.store(false);
            }
            return *this;
        }
    };

    std::vector<Entry> m_pool;
    std::atomic<size_t> m_nextIndex{0};

    ID3D12Device *m_device = nullptr;
    std::mutex m_expandMutex; // 仅用于保护 vector 扩容

    void Expand(size_t newSize);
};

// ========================================================================
// 3. 显式实例化声明
// 告诉编译器：这些特定类型的实现将在其他地方（.cpp）提供
// ========================================================================

extern template class CommandAllocatorPool<D3D12_COMMAND_LIST_TYPE_DIRECT>;
extern template class CommandAllocatorPool<D3D12_COMMAND_LIST_TYPE_COMPUTE>;
extern template class CommandAllocatorPool<D3D12_COMMAND_LIST_TYPE_COPY>;

} // namespace DX12Engine::Renderer