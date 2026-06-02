#pragma once

#include "Renderer/RHI/Command/Allocator/CommandAllocatorPool.h"
#include "Renderer/RHI/Command/CommandList/CommandListPool.h"
#include "Resource/Struct/ResourceHandle.h"
#include "Scheduler/Task.h"
#include "Scheduler/TaskGraph.h"
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <taskflow/taskflow.hpp>
#include <vector>

namespace DX12Engine::Renderer {
class CommandManager;
}

namespace DX12Engine::Async {

// ============================================================================
// GPU 工作单元 — 后台线程录制完成后，将句柄写入此结构
// ============================================================================
struct GpuWorkItem {
    // COPY 队列录制结果
    Renderer::CommandListPool<D3D12_COMMAND_LIST_TYPE_COPY>::Handle copyCmdListHandle;
    Renderer::CommandAllocatorPool<D3D12_COMMAND_LIST_TYPE_COPY>::Handle copyAllocatorHandle;

    // DIRECT 队列录制结果
    Renderer::CommandListPool<D3D12_COMMAND_LIST_TYPE_DIRECT>::Handle directCmdListHandle;
    Renderer::CommandAllocatorPool<D3D12_COMMAND_LIST_TYPE_DIRECT>::Handle directAllocatorHandle;

    // 上传缓冲区（COPY 提交后可释放）
    Resource::GpuResourceHandle uploadBufferHandle = Resource::GpuResourceHandle::Invalid();

    // GPU 提交完成后的回调（主线程执行）
    // 参数: bool success
    std::function<void(bool)> onComplete;

    // 标识：此项是否有效
    std::atomic<bool> ready{false};
};

using GpuWorkItemPtr = std::shared_ptr<GpuWorkItem>;

/**
 * @brief 后台任务执行器 — 承担类似帧驱动器的能力
 *
 * 设计类比 FrameDriver：
 *   FrameDriver:   主线程 Tick → System录制命令 → SubmitRenderCommand → ExecuteRenderPhase(批量Submit)
 *   BackgroundExecutor: 后台线程录制命令 → 写入 GpuWorkItem
 *                      主线程 Tick → 收集 GpuWorkItem → 统一Submit(COPY→DIRECT) → 回调
 *
 * 数据流：
 *   后台线程: 加载文件 → 创建GPU资源 → 录制命令列表(Close不Submit) → 写入GpuWorkItem
 *   主线程Tick: 收集就绪的GpuWorkItem → Submit COPY → Signal COPY fence
 *              → Submit DIRECT(Wait COPY fence) → Signal DIRECT fence
 *              → 等待 DIRECT fence → 调用 onComplete 回调
 */
class BackgroundExecutor {
public:
    /**
     * @brief 构造后台执行器
     * @param threadCount 工作线程数（默认 2）
     */
    explicit BackgroundExecutor(size_t threadCount = 2);
    ~BackgroundExecutor();

    // 禁止拷贝
    BackgroundExecutor(const BackgroundExecutor &) = delete;
    BackgroundExecutor &operator=(const BackgroundExecutor &) = delete;

    // ========================================================================
    // System 注册（类比 FrameDriver::RegisterImmediateCallback）
    // ========================================================================

    /// 设置命令管理器引用（主线程调用）
    void SetCommandManager(Renderer::CommandManager *cmdMgr) { m_cmdMgr = cmdMgr; }

    // ========================================================================
    // 任务提交（后台线程调用）
    // ========================================================================

    void Submit(Scheduler::Task task);
    void SubmitGraph(Scheduler::TaskGraph graph);

    // ========================================================================
    // GPU 工作项注册（后台线程调用，将录好的命令注册到队列）
    // ========================================================================

    /**
     * @brief 注册一个 GPU 工作项（后台线程完成录制后调用）
     * @param item 包含已录制的命令列表句柄和完成回调
     */
    void RegisterGpuWork(GpuWorkItemPtr item);

    // ========================================================================
    // 主线程 Tick（类似 FrameDriver::Tick）
    // ========================================================================

    /**
     * @brief 每帧主线程调用
     *
     * 执行流程（类比 FrameDriver::Tick）：
     *   1. 清理已完成的 CPU taskflow（原有逻辑）
     *   2. 收集就绪的 GpuWorkItem
     *   3. 按序提交：Submit COPY → Signal COPY → Submit DIRECT(Wait COPY) → Signal DIRECT
     *   4. 等待 DIRECT fence → 调用 onComplete 回调 → 释放命令列表/分配器
     */
    void Tick();

    void WaitAll();

    size_t GetPendingCount() const { return m_pendingCount.load(std::memory_order_acquire); }
    size_t GetTotalSubmitted() const { return m_totalSubmitted.load(std::memory_order_acquire); }
    size_t GetTotalCompleted() const { return m_totalCompleted.load(std::memory_order_acquire); }
    size_t GetGpuWorkQueueSize() const;

    tf::Executor &GetExecutor() { return m_executor; }

private:
    struct PendingTaskflow {
        std::unique_ptr<tf::Taskflow> taskflow;
        tf::Future<void> future;
        size_t taskCount = 0;
    };

    tf::Executor m_executor;

    std::mutex m_pendingMutex;
    std::vector<PendingTaskflow> m_pending;

    // GPU 工作队列（后台线程写入，主线程 Tick 中消费）
    mutable std::mutex m_gpuWorkMutex;
    std::vector<GpuWorkItemPtr> m_gpuWorkQueue;

    std::atomic<size_t> m_pendingCount{0};
    std::atomic<size_t> m_totalSubmitted{0};
    std::atomic<size_t> m_totalCompleted{0};

    Renderer::CommandManager *m_cmdMgr = nullptr;

    /// 处理单个 GPU 工作项（主线程调用）
    void ProcessGpuWork(const GpuWorkItemPtr &item);
};

} // namespace DX12Engine::Async
