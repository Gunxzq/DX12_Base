#pragma once

#include "Scheduler/Task.h"
#include "Scheduler/TaskGraph.h"
#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <taskflow/taskflow.hpp>
#include <vector>

namespace DX12Engine::Async {

/**
 * @brief 后台任务执行器 — 独立于 FrameDriver 的 CPU 线程池
 * 数据流：
 *   后台线程: 加载文件 → 解析数据 → 写入 DataPool → 发送 ResourceLoaded 事件
 *   主线程:   UploadSystem 收到事件 → 编码 COPY 命令 → 提交 → 轮询围栏
 */
class BackgroundExecutor {
public:
    /**
     * @brief 构造后台执行器
     * @param threadCount 工作线程数（默认 2，后台任务不需要太多线程）
     */
    explicit BackgroundExecutor(size_t threadCount = 2);
    ~BackgroundExecutor();

    // 禁止拷贝
    BackgroundExecutor(const BackgroundExecutor &) = delete;
    BackgroundExecutor &operator=(const BackgroundExecutor &) = delete;

    void Submit(Scheduler::Task task);
    void SubmitGraph(Scheduler::TaskGraph graph);
    void Tick();

    void WaitAll();

    size_t GetPendingCount() const { return m_pendingCount.load(std::memory_order_acquire); }
    size_t GetTotalSubmitted() const { return m_totalSubmitted.load(std::memory_order_acquire); }
    size_t GetTotalCompleted() const { return m_totalCompleted.load(std::memory_order_acquire); }

    tf::Executor &GetExecutor() { return m_executor; }

private:
    struct PendingTaskflow {
        std::unique_ptr<tf::Taskflow> taskflow;
        tf::Future<void> future; // 用于检查完成状态
        size_t taskCount = 0;
    };

    tf::Executor m_executor; // 独立的工作线程池

    std::mutex m_pendingMutex;
    std::vector<PendingTaskflow> m_pending; // 正在执行的任务流

    std::atomic<size_t> m_pendingCount{0};
    std::atomic<size_t> m_totalSubmitted{0};
    std::atomic<size_t> m_totalCompleted{0};
};

} // namespace DX12Engine::Async
