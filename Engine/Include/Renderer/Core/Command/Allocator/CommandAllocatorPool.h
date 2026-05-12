#pragma once

#include "CommandAllocator.h"
#include <atomic>
#include <cassert>
#include <d3d12.h>
#include <memory>
#include <mutex>
#include <vector>

namespace DX12Engine::Renderer {

/**
 * @brief 线程安全的无锁命令分配器池
 * @tparam Type 命令列表类型
 */
template <D3D12_COMMAND_LIST_TYPE Type> class CommandAllocatorPool {
public:
    // 句柄：用于快速 Release，避免遍历查找
    struct Handle {
        size_t index = static_cast<size_t>(-1);
        CommandAllocator<Type> *allocator = nullptr;

        bool IsValid() const { return index != static_cast<size_t>(-1); }
    };

    explicit CommandAllocatorPool(ID3D12Device *device, size_t initialSize = 8) : m_device(device) {
        Expand(initialSize);
    }

    ~CommandAllocatorPool() = default;

    CommandAllocatorPool(const CommandAllocatorPool &) = delete;
    CommandAllocatorPool &operator=(const CommandAllocatorPool &) = delete;

    /**
     * @brief 获取一个可用的命令分配器
     * @param currentGpuCompletedValue 当前 GPU 已完成的 Fence 值
     * @return Handle
     */
    Handle Acquire(uint64_t currentGpuCompletedValue) {
        size_t poolSize = m_pool.size();

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

        // 5. 所有分配器都不可用（正在使用或 GPU 未完成）
        // 策略：扩容。这是处理突发并发的最安全方式。
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

    /**
     * @brief 释放分配器
     * @param handle Acquire 返回的句柄
     * @param fenceValue 本次提交关联的 Fence 值
     */
    void Release(const Handle &handle, uint64_t fenceValue) {
        assert(handle.IsValid() && "Invalid Handle released");

        // O(1) 直接通过索引访问
        Entry &entry = m_pool[handle.index];

        // 更新 Fence 值 (原子写入)
        entry.lastFenceValue.store(fenceValue, std::memory_order_release);

        // 标记为未使用 (原子写入，允许其他线程再次 Acquire)
        entry.inUse.store(false, std::memory_order_release);
    }

private:
    // 缓存行大小，防止伪共享
    static constexpr size_t CACHE_LINE_SIZE = 64;

    struct alignas(CACHE_LINE_SIZE) Entry {
        std::unique_ptr<CommandAllocator<Type>> allocator;
        std::atomic<uint64_t> lastFenceValue{0};
        std::atomic<bool> inUse{false};

        // 填充字节，确保每个 Entry 独占一个或多个缓存行
        char padding[CACHE_LINE_SIZE - sizeof(std::unique_ptr<CommandAllocator<Type>>) - sizeof(std::atomic<uint64_t>) -
                     sizeof(std::atomic<bool>)];
    };

    std::vector<Entry> m_pool;
    std::atomic<size_t> m_nextIndex{0};

    ID3D12Device *m_device = nullptr;
    std::mutex m_expandMutex; // 仅用于保护 vector 扩容

    void Expand(size_t newSize) {
        size_t oldSize = m_pool.size();
        m_pool.resize(newSize);

        for (size_t i = oldSize; i < newSize; ++i) {
            m_pool[i].allocator = std::make_unique<CommandAllocator<Type>>(m_device);
            m_pool[i].lastFenceValue.store(0, std::memory_order_relaxed);
            m_pool[i].inUse.store(false, std::memory_order_relaxed);
        }
    }

public:
    // 统计信息
    struct Stats {
        size_t totalCount = 0;
        size_t inUseCount = 0;
    };

    /// 获取池的统计信息
    Stats GetStats() const {
        Stats stats;
        stats.totalCount = m_pool.size();
        for (const auto &entry : m_pool) {
            if (entry.inUse.load(std::memory_order_relaxed)) {
                ++stats.inUseCount;
            }
        }
        return stats;
    }
};

} // namespace DX12Engine::Renderer