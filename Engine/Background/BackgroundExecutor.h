#pragma once

#include "Renderer/RHI/Command/Allocator/CommandAllocatorPool.h"
#include "Renderer/RHI/Command/CommandList/CommandListPool.h"
#include "Resource/Core/GpuHandlePool.h"
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

    // 上传缓冲区句柄列表（COPY 提交后可释放）
    std::vector<Resource::GpuResourceHandle> uploadBufferHandles;

    // GPU 提交完成后的回调（主线程执行）
    // 参数: bool success
    std::function<void(bool)> onComplete;

    // 标识：此项是否有效
    std::atomic<bool> ready{false};

    // ── 围栏值（ProcessGpuWork Signal 后写入，供外部查询 GPU 完成状态） ──
    uint64_t copyFenceValue = 0;   // COPY 队列 Signal fence
    uint64_t directFenceValue = 0; // DIRECT 队列 Signal fence
};

using GpuWorkItemPtr = std::shared_ptr<GpuWorkItem>;

// ============================================================================
// 通用加载任务 — 表达"CPU 工作 → GPU 工作 → 完成回调"的完整链路
//
// 数据流：
//   SubmitLoadTask(task)
//     → background thread: cpuWork() 执行 CPU 加载/创建 GPU 资源
//     → background thread: gpuWork() 录制 COPY+DIRECT 命令，返回 GpuWorkItem
//     → main thread Tick:   提交 COPY → Signal → DIRECT(Wait) → Signal
//     → main thread:        onComplete(success) 回调
//
// 灵活性：
//   不同资源类型只需替换 cpuWork / gpuWork 函数，不需要新增 Factory 类。
//   复合资产（如地形）用 SubmitGraph 表达依赖，每个叶子节点是独立 LoadTask。
// ============================================================================

struct LoadTask {
    std::string name;

    // Step 1: 后台线程执行（CPU 加载、解析、创建 GPU 资源）
    // 返回值：后续需要上传到 GPU 的数据
    std::function<void()> cpuWork;

    // Step 2: 后台线程执行（录制 COPY+DIRECT 命令，返回 GpuWorkItem）
    // 在 cpuWork 完成后自动调用
    std::function<GpuWorkItemPtr()> gpuWork;

    // Step 3: 主线程执行（GPU 上传完成后回调）
    std::function<void(bool success)> onComplete;
};

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
 *              → 非阻塞检查 DIRECT fence → 调用 onComplete 回调
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
    // 通用加载任务提交
    // ========================================================================

    /**
     * @brief 提交一个通用加载任务
     *
     * 内部将 LoadTask 拆为两步：
     *   1. CPU 工作 → 包装为 Scheduler::Task 提交到后台线程
     *   2. CPU 完成后自动调用 gpuWork() → RegisterGpuWork
     *   3. Tick 中 GPU 完成 → onComplete 回调
     */
    void SubmitLoadTask(LoadTask task);

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
     *   3. 按序提交：Submit COPY → Signal COPY → Submit DIRECT(Wait COPY) → Signal DIRECT（不阻塞）
     *   4. 非阻塞检查 pending DIRECT fence → 释放上传缓冲区 → 调用 onComplete
     *   5. 执行延后的主线程回调（纯 CPU 任务）
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

    // ── GPU 待完成队列（ProcessGpuWork Submit 后不阻塞，移入此队列） ──
    struct PendingCompletion {
        GpuWorkItemPtr item;
        uint64_t directFenceValue = 0;
    };
    mutable std::mutex m_pendingCompletionMutex;
    std::vector<PendingCompletion> m_pendingCompletion;

    // ── 主线程延后回调队列（无 gpuWork 任务的后处理） ──
    mutable std::mutex m_deferredMutex;
    std::vector<std::function<void()>> m_deferredCallbacks;

    Renderer::CommandManager *m_cmdMgr = nullptr;

    /// 处理单个 GPU 工作项（主线程调用，只 Submit + Signal，不阻塞）
    void ProcessGpuWork(const GpuWorkItemPtr &item);

    /// 非阻塞检查 GPU 完成（Tick Phase 4）
    void CheckPendingCompletions();

    /// 延后回调到主线程（无 gpuWork 任务用）
    void DeferToMainThread(std::function<void()> callback);

    /// 执行延后的主线程回调（Tick Phase 5）
    void ExecuteDeferredCallbacks();
};

} // namespace DX12Engine::Async
