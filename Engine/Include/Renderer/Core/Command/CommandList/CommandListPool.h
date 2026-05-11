#pragma once

#include "CommandList.h"
#include <atomic>
#include <cassert>
#include <d3d12.h>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <vector>

namespace DX12Engine::Renderer {

/**
 * @brief 线程安全的命令列表池
 * @tparam Type 命令列表类型 (DIRECT, COMPUTE, COPY)
 */
template <D3D12_COMMAND_LIST_TYPE Type> class CommandListPool {
    static_assert(Type == D3D12_COMMAND_LIST_TYPE_DIRECT || Type == D3D12_COMMAND_LIST_TYPE_COMPUTE ||
                      Type == D3D12_COMMAND_LIST_TYPE_COPY,
                  "Invalid D3D12 Command List Type");

public:
    // 句柄：用于 Release
    struct Handle {
        size_t index = static_cast<size_t>(-1);

        bool IsValid() const { return index != static_cast<size_t>(-1); }
    };

    explicit CommandListPool(ID3D12Device *device, size_t initialSize = 8) : m_device(device), m_type(Type) {
        Expand(initialSize);
    }

    ~CommandListPool() = default;

    CommandListPool(const CommandListPool &) = delete;
    CommandListPool &operator=(const CommandListPool &) = delete;

    /**
     * @brief 获取一个命令列表句柄
     * @return Handle
     * @note 获取后必须调用 Reset 才能使用
     */
    Handle AcquireHandle() {
        while (true) {
            size_t poolSize = m_pool.size();
            // 防止 poolSize 为 0 (虽然 Expand 保证了至少有一个)
            if (poolSize == 0) {
                std::lock_guard<std::mutex> lock(m_expandMutex);
                Expand(1);
                continue;
            }

            size_t startIdx = m_nextIndex.fetch_add(1, std::memory_order_relaxed) % poolSize;

            for (size_t i = 0; i < poolSize; ++i) {
                size_t idx = (startIdx + i) % poolSize;
                Entry &entry = m_pool[idx];

                bool expectedInUse = false;
                if (entry.inUse.compare_exchange_strong(expectedInUse, true, std::memory_order_acquire)) {
                    return {idx};
                }
            }

            // 所有资源都在使用中，尝试扩容
            {
                std::lock_guard<std::mutex> lock(m_expandMutex);
                // 双重检查，防止多线程同时扩容
                if (m_pool.size() == poolSize) {
                    Expand(poolSize * 2);
                }
            }
            // 循环重试，而不是递归，避免栈溢出
        }
    }

    /**
     * @brief 通过句柄获取 CommandList 封装对象
     */
    CommandList GetCommandList(const Handle &handle) {
        assert(handle.IsValid());
        return CommandList(m_pool[handle.index].cmdList.Get());
    }

    /**
     * @brief 释放命令列表
     * @param handle 之前 AcquireHandle 得到的句柄
     * @note 释放后，该 CommandList 对象在下次 Acquire 前不应再被使用
     */
    void Release(const Handle &handle) {
        assert(handle.IsValid());
        Entry &entry = m_pool[handle.index];

        // 可选：调试模式下检查是否真的被标记为 inUse
        // assert(entry.inUse.load() == true);

        entry.inUse.store(false, std::memory_order_release);
    }

private:
    static constexpr size_t CACHE_LINE_SIZE = 64;

    struct alignas(CACHE_LINE_SIZE) Entry {
        Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> cmdList;
        std::atomic<bool> inUse{false};

        // 更安全的 Padding 计算
        // 确保整个结构体大小是 CACHE_LINE_SIZE 的倍数，或者至少独占一行
        // 这里简单处理：如果 sizeof 小于 CACHE_LINE_SIZE，则填充剩余部分
        static constexpr size_t MemberSize =
            sizeof(Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList>) + sizeof(std::atomic<bool>);
        static constexpr size_t PadSize = (MemberSize < CACHE_LINE_SIZE) ? (CACHE_LINE_SIZE - MemberSize) : 0;
        char padding[PadSize];
    };

    std::vector<Entry> m_pool;
    std::atomic<size_t> m_nextIndex{0};

    ID3D12Device *m_device = nullptr;
    D3D12_COMMAND_LIST_TYPE m_type;
    std::mutex m_expandMutex;

    void Expand(size_t newSize) {
        size_t oldSize = m_pool.size();
        m_pool.resize(newSize);

        for (size_t i = oldSize; i < newSize; ++i) {
            HRESULT hr = m_device->CreateCommandList(0, m_type, nullptr, nullptr, IID_PPV_ARGS(&m_pool[i].cmdList));

            if (FAILED(hr)) {
                throw std::runtime_error("Failed to create CommandList in Pool");
            }

            // 初始必须 Close，才能后续 Reset
            m_pool[i].cmdList->Close();

            m_pool[i].inUse.store(false, std::memory_order_relaxed);
        }
    }
};

} // namespace DX12Engine::Renderer