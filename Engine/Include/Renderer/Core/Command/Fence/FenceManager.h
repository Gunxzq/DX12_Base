#pragma once

#include "Fence.h"
#include <atomic>
#include <cstdint>
#include <shared_mutex>
#include <unordered_map>

namespace DX12Engine {
namespace Renderer {

// ========================================================================
// FenceManager - 围栏对象管理器
// 职责：
// 1. 管理多个类型的围栏对象 (每个 Command List Type 对应一个 Fence)
// 2. 提供全局唯一序号生成器 (用于标记 Command List 的生命周期)
// 3. 提供基于全局序号的等待机制 (CPU 端阻塞等待)
//
// 注意：
// - m_globalSequence 是 CPU 端的“发号器”，只增不减。
// - 所有的 Signal/Wait/Check 逻辑均由 Manager 直接调用 DX12 API 实现。
// ========================================================================

class FenceManager {
public:
    FenceManager() = default;
    ~FenceManager() = default;

    // 禁止拷贝和移动
    FenceManager(const FenceManager &) = delete;
    FenceManager &operator=(const FenceManager &) = delete;
    FenceManager(FenceManager &&) = delete;
    FenceManager &operator=(FenceManager &&) = delete;

    // ========================================================================
    // 初始化
    // ========================================================================

    /**
     * @brief 创建指定类型的围栏
     * @param device D3D12 设备
     * @param type 命令列表类型
     */
    void CreateFence(ID3D12Device *device, D3D12_COMMAND_LIST_TYPE type) {
        std::unique_lock lock(m_mutex);
        // 如果已存在，先清理旧的（可选，视具体需求而定）
        // m_fences.erase(type);
        m_fences[type] = std::make_unique<Fence>(device, 0);
    }

    // ========================================================================
    // 全局序号生成器 (核心接口)
    // ========================================================================

    /**
     * @brief 获取下一个全局唯一序号 (期望值)
     * @note
     * - 此值是单调递增的，用于标记资源或命令列表的版本/生命周期。
     * - 不需要与 GPU 状态同步，它仅代表 CPU 端的提交顺序。
     */
    uint64_t GetNextSequence() { return m_globalSequence.fetch_add(1, std::memory_order_relaxed); }

    // ========================================================================
    // 围栏操作
    // ========================================================================

    /**
     * @brief 在指定队列上发出信号
     * @param type 命令列表类型
     * @param queue 命令队列
     * @return uint64_t 本次信号使用的全局序号
     */
    uint64_t Signal(D3D12_COMMAND_LIST_TYPE type, ID3D12CommandQueue *queue, uint64_t value) {
        Fence *fence = GetFence(type);
        if (!fence) {
            return 0;
        }

        queue->Signal(fence->Get(), value);

        return value;
    }

    /**
     * @brief 获取指定类型的围栏指针 (仅供内部或高级用途使用)
     */
    Fence *GetFence(D3D12_COMMAND_LIST_TYPE type) {
        std::shared_lock lock(m_mutex);
        auto it = m_fences.find(type);
        return (it != m_fences.end()) ? it->second.get() : nullptr;
    }

    // ========================================================================
    // 完成状态查询
    // ========================================================================

    /**
     * @brief 检查特定队列上的指定序号是否已完成
     * @param type 命令列表类型
     * @param sequence 要检查的全局序号
     * @return true 如果 GPU 已经执行完该序号对应的 Signal
     */
    bool IsSequenceCompleted(D3D12_COMMAND_LIST_TYPE type, uint64_t sequence) {
        Fence *fence = GetFence(type);
        if (!fence) {
            return false;
        }

        // 直接查询底层 Fence 的完成值
        return fence->Get()->GetCompletedValue() >= sequence;
    }

    /**
     * @brief 等待特定队列上的指定序号完成 (CPU 阻塞)
     * @param type 命令列表类型
     * @param sequence 要等待的全局序号
     */
    void WaitForSequence(D3D12_COMMAND_LIST_TYPE type, uint64_t sequence) {
        Fence *fence = GetFence(type);
        if (!fence) {
            return;
        }

        ID3D12Fence *fencePtr = fence->Get();
        HANDLE eventHandle = fence->GetEventHandle();

        // 快速路径：如果已经完成，直接返回，避免系统调用开销
        if (fencePtr->GetCompletedValue() >= sequence) {
            return;
        }

        // 慢速路径：
        // 1. 设置当 Fence 达到指定值时触发事件
        ThrowIfFailed(fencePtr->SetEventOnCompletion(sequence, eventHandle));

        // 2. 阻塞当前线程直到事件被触发
        WaitForSingleObject(eventHandle, INFINITE);
    }

    /**
     * @brief 关闭/清理所有围栏
     */
    void Shutdown() {
        std::unique_lock lock(m_mutex);
        m_fences.clear();
        // m_globalSequence 保持不动，防止重启后序列号冲突（如果引擎不彻底退出的话）
    }

private:
    mutable std::shared_mutex m_mutex;

    // 每个命令队列类型对应一个 Fence 对象
    std::unordered_map<D3D12_COMMAND_LIST_TYPE, std::unique_ptr<Fence>> m_fences;

    // 全局序号生成器：单调递增，线程安全
    std::atomic<uint64_t> m_globalSequence{1};
};

} // namespace Renderer
} // namespace DX12Engine