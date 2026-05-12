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

// ========================================================================
// 1. 非模板基类接口
// ========================================================================

class ICommandListPool {
public:
    virtual ~ICommandListPool() = default;
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
 * @brief 线程安全的命令列表池
 * @tparam Type 命令列表类型 (DIRECT, COMPUTE, COPY)
 */
template <D3D12_COMMAND_LIST_TYPE Type> class CommandListPool : public ICommandListPool {
    static_assert(Type == D3D12_COMMAND_LIST_TYPE_DIRECT || Type == D3D12_COMMAND_LIST_TYPE_COMPUTE ||
                      Type == D3D12_COMMAND_LIST_TYPE_COPY,
                  "Invalid D3D12 Command List Type");

public:
    // 句柄：用于 Release
    struct Handle {
        size_t index = static_cast<size_t>(-1);

        bool IsValid() const { return index != static_cast<size_t>(-1); }
    };

    explicit CommandListPool(ID3D12Device *device);
    ~CommandListPool() override;

    CommandListPool(const CommandListPool &) = delete;
    CommandListPool &operator=(const CommandListPool &) = delete;

    // 实现基类接口
    void Shutdown() override;
    typename ICommandListPool::Stats GetStats() const override;

    /**
     * @brief 获取一个命令列表句柄
     * @return Handle
     * @note 获取后必须调用 Reset 才能使用
     */
    Handle AcquireHandle(ID3D12CommandAllocator *allocator);

    /**
     * @brief 通过句柄获取 CommandList 封装对象
     */
    CommandList GetCommandList(const Handle &handle);

    /**
     * @brief 释放命令列表
     * @param handle 之前 AcquireHandle 得到的句柄
     * @note 释放后，该 CommandList 对象在下次 Acquire 前不应再被使用
     */
    void Release(const Handle &handle);

private:
    static constexpr size_t CACHE_LINE_SIZE = 64;

    struct alignas(CACHE_LINE_SIZE) Entry {
        Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> cmdList;
        std::atomic<bool> inUse{false};

        // 更安全的 Padding 计算
        static constexpr size_t MemberSize =
            sizeof(Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList>) + sizeof(std::atomic<bool>);
        static constexpr size_t PadSize = (MemberSize < CACHE_LINE_SIZE) ? (CACHE_LINE_SIZE - MemberSize) : 0;
        char padding[PadSize];

        // 默认构造
        Entry() = default;

        // 禁止拷贝
        Entry(const Entry &) = delete;
        Entry &operator=(const Entry &) = delete;

        // 支持移动
        Entry(Entry &&other) noexcept : cmdList(std::move(other.cmdList)), inUse(other.inUse.load()) {
            other.inUse.store(false);
        }

        Entry &operator=(Entry &&other) noexcept {
            if (this != &other) {
                cmdList = std::move(other.cmdList);
                inUse.store(other.inUse.load());
                other.inUse.store(false);
            }
            return *this;
        }
    };

    std::vector<Entry> m_pool;
    std::atomic<size_t> m_nextIndex{0};

    ID3D12Device *m_device = nullptr;
    D3D12_COMMAND_LIST_TYPE m_type;
    std::mutex m_expandMutex;
};

// ========================================================================
// 3. 显式实例化声明
// ========================================================================

extern template class CommandListPool<D3D12_COMMAND_LIST_TYPE_DIRECT>;
extern template class CommandListPool<D3D12_COMMAND_LIST_TYPE_COMPUTE>;
extern template class CommandListPool<D3D12_COMMAND_LIST_TYPE_COPY>;

} // namespace DX12Engine::Renderer