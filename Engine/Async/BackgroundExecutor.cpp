#include "BackgroundExecutor.h"
#include <algorithm>

namespace DX12Engine::Async {

BackgroundExecutor::BackgroundExecutor(size_t threadCount)
    : m_executor(threadCount > 0 ? threadCount : 2) {
}

BackgroundExecutor::~BackgroundExecutor() {
    WaitAll();
}

void BackgroundExecutor::Submit(Scheduler::Task task) {
    if (!task.execute) return;

    auto tf = std::make_unique<tf::Taskflow>();
    tf->emplace([task = std::move(task)]() mutable {
        task.execute();
    });

    m_totalSubmitted.fetch_add(1, std::memory_order_relaxed);
    m_pendingCount.fetch_add(1, std::memory_order_release);

    // 异步运行，不阻塞
    auto future = m_executor.run(*tf);

    PendingTaskflow pending;
    pending.taskflow = std::move(tf);
    pending.future = std::move(future);
    pending.taskCount = 1;

    {
        std::lock_guard<std::mutex> lock(m_pendingMutex);
        m_pending.push_back(std::move(pending));
    }
}

void BackgroundExecutor::SubmitGraph(Scheduler::TaskGraph graph) {
    const auto& tasks = graph.GetTasks();
    if (tasks.empty()) return;

    auto tf = std::make_unique<tf::Taskflow>();
    std::vector<tf::Task> tfTasks;
    tfTasks.reserve(tasks.size());
    size_t taskCount = 0;

    // 将所有有 execute 的任务添加到 taskflow
    for (const auto& task : tasks) {
        if (!task.execute) continue;

        auto tfTask = tf->emplace([task = task]() mutable {
            task.execute();
        });
        tfTasks.push_back(tfTask);
        taskCount++;
    }

    // 设置依赖关系
    for (size_t i = 0; i < tasks.size(); ++i) {
        const auto& task = tasks[i];
        if (task.dependencies.empty()) continue;

        for (auto depId : task.dependencies) {
            for (size_t j = 0; j < tasks.size(); ++j) {
                if (tasks[j].id == depId && j < tfTasks.size() && i < tfTasks.size()) {
                    tfTasks[j].precede(tfTasks[i]);
                    break;
                }
            }
        }
    }

    if (taskCount == 0) return;

    m_totalSubmitted.fetch_add(taskCount, std::memory_order_relaxed);
    m_pendingCount.fetch_add(taskCount, std::memory_order_release);

    auto future = m_executor.run(*tf);

    PendingTaskflow pending;
    pending.taskflow = std::move(tf);
    pending.future = std::move(future);
    pending.taskCount = taskCount;

    {
        std::lock_guard<std::mutex> lock(m_pendingMutex);
        m_pending.push_back(std::move(pending));
    }
}

void BackgroundExecutor::Tick() {
    std::lock_guard<std::mutex> lock(m_pendingMutex);

    auto it = m_pending.begin();
    while (it != m_pending.end()) {
        // 检查 future 是否已完成（0 时长等待 = 非阻塞检查）
        if (it->future.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
            m_pendingCount.fetch_sub(it->taskCount, std::memory_order_release);
            m_totalCompleted.fetch_add(it->taskCount, std::memory_order_relaxed);
            it = m_pending.erase(it);
        } else {
            ++it;
        }
    }
}

void BackgroundExecutor::WaitAll() {
    m_executor.wait_for_all();

    std::lock_guard<std::mutex> lock(m_pendingMutex);
    for (auto& pending : m_pending) {
        m_pendingCount.fetch_sub(pending.taskCount, std::memory_order_release);
        m_totalCompleted.fetch_add(pending.taskCount, std::memory_order_relaxed);
    }
    m_pending.clear();
}

} // namespace DX12Engine::Async
