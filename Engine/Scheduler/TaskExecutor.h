#pragma once
#include "TaskGraph.h"
#include <functional>
#include <future>
#include <taskflow/taskflow.hpp>
#include <vector>

namespace DX12Engine::Scheduler {

// ========================================================================
// TaskFlow 执行器
// ========================================================================

/**
 * @brief L3 核心：基于 TaskFlow 的任务调度与执行
 *
 * 职责：
 * 1. 使用 tf::Executor 管理工作线程（自带 Work Stealing）
 * 2. 将 TaskGraph 转换为 tf::Task
 * 3. 管理任务依赖（使用 TaskFlow 的 precede）
 * 4. 同步点：等待某阶段所有任务完成
 *
 * 设计原则：
 * - 不重复造轮子：Work Stealing 直接使用 TaskFlow 实现
 * - 预留插口：Frame Slicing 暂时不实现，但预留接口
 * - 线程安全：TaskFlow 内部保证
 *
 * 执行策略：
 * - Any/Worker：使用 TaskFlow 并行执行
 * - Main：收集到主线程队列（当前帧稍后执行）
 * - Render：收集到渲染线程队列
 */
class TaskExecutor {
public:
    /**
     * @brief 构造执行器
     * @param workerCount 工作线程数（0 = 自动，使用硬件并发数）
     */
    explicit TaskExecutor(size_t workerCount = 0);
    ~TaskExecutor() = default;

    // 禁止拷贝，允许移动
    TaskExecutor(const TaskExecutor &) = delete;
    TaskExecutor &operator=(const TaskExecutor &) = delete;
    TaskExecutor(TaskExecutor &&) noexcept = default;
    TaskExecutor &operator=(TaskExecutor &&) noexcept = default;

    /// 执行整个任务图
    /// @param graph 已构建的任务图（DAG）
    void Execute(const TaskGraph &graph);

    /// 执行指定阶段的任务
    /// @param graph 任务图
    /// @param phase 执行阶段
    void ExecutePhase(const TaskGraph &graph, TaskPhase phase);

    /// 等待所有任务完成（屏障同步）
    void WaitForCompletion();

    /// 获取 TaskFlow Executor（高级用法）
    tf::Executor &GetFlowExecutor() { return m_executor; }

    /// 获取主线程任务队列（由 FrameDriver 消费）
    std::vector<std::function<void()>> StealMainThreadTasks();

    /// 获取渲染线程任务队列（由 RenderThread 消费）
    std::vector<std::function<void()>> StealRenderThreadTasks();

    /// 【预留插口】获取动态任务上限（用于 Frame Slicing）
    /// @return 当前帧允许的最大任务数（0 = 无限制）
    size_t GetDynamicTaskCap() const;

    /// 【预留插口】设置是否有未完成的任务积压
    /// @param hasBacklog 是否有积压
    void SetBacklogFlag(bool hasBacklog) { m_hasBacklog = hasBacklog; }

    /// 【预留插口】检查是否有积压任务
    bool HasBacklog() const { return m_hasBacklog; }

private:
    tf::Executor m_executor; // TaskFlow 执行器（自带 Work Stealing）
    tf::Taskflow m_taskflow; // 当前帧的任务流

    std::mutex m_mainThreadMutex;
    std::mutex m_renderThreadMutex;
    std::vector<std::function<void()>> m_mainThreadQueue;
    std::vector<std::function<void()>> m_renderThreadQueue;

    bool m_hasBacklog = false; // 【预留】是否有积压任务

    /// 将 Task 分发到对应队列
    void DispatchToThreadQueue(const Task &task);
};

} // namespace DX12Engine::Scheduler
