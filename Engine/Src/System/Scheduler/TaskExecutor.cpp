#include "System/Scheduler/TaskExecutor.h"
#include <cassert>

namespace DX12::Scheduler {

// ========================================================================
// TaskExecutor Implementation (TaskFlow Version)
// ========================================================================

TaskExecutor::TaskExecutor(size_t workerCount)
    : m_executor(workerCount == 0 ? std::thread::hardware_concurrency() : workerCount)
{
}

void TaskExecutor::Execute(const TaskGraph& graph) {
    // 清空上一帧的任务流
    m_taskflow.clear();

    // 获取按阶段分组的拓扑排序结果
    auto phaseGroups = graph.SortByPhase();

    // 存储 tf::Task 和 TaskId 的映射（用于建立依赖）
    std::unordered_map<TaskId, tf::Task> flowTasks;

    // 按阶段顺序创建 tf::Task
    for (int phaseIdx = 0; phaseIdx < static_cast<int>(TaskPhase::Count); ++phaseIdx) {
        TaskPhase phase = static_cast<TaskPhase>(phaseIdx);
        auto it = phaseGroups.find(phase);
        if (it == phaseGroups.end()) continue;

        const auto& taskIds = it->second;

        // 创建 tf::Task
        for (TaskId id : taskIds) {
            const Task* task = graph.GetTask(id);
            if (!task) continue;

            // Main/Render 线程任务：分发到专用队列
            if (task->thread == ThreadType::Main || task->thread == ThreadType::Render) {
                DispatchToThreadQueue(*task);
                continue;
            }

            // Any/Worker 任务：创建 tf::Task
            tf::Task flowTask = m_taskflow.emplace([func = task->execute]() {
                func();
            });
            flowTask.name(task->name.c_str());
            flowTasks[id] = flowTask;
        }

        // 建立依赖关系
        for (TaskId id : taskIds) {
            const Task* task = graph.GetTask(id);
            if (!task || task->thread != ThreadType::Any) continue;

            auto flowTaskIt = flowTasks.find(id);
            if (flowTaskIt == flowTasks.end()) continue;

            for (TaskId depId : task->dependencies) {
                auto depIt = flowTasks.find(depId);
                if (depIt != flowTasks.end()) {
                    flowTaskIt->second.succeed(depIt->second);
                }
            }
        }
    }

    // 【预留插口】Frame Slicing 检查
    // size_t taskCap = GetDynamicTaskCap();
    // if (taskCap > 0 && flowTasks.size() > taskCap) {
    //     // 任务过多，需要分帧
    //     SetBacklogFlag(true);
    // }

    // 执行 TaskFlow（自动 Work Stealing）
    if (!m_taskflow.empty()) {
        m_executor.run(m_taskflow).wait();
    }
}

void TaskExecutor::ExecutePhase(const TaskGraph& graph, TaskPhase phase) {
    // 清空任务流
    m_taskflow.clear();

    // 获取指定阶段的任务
    auto phaseGroups = graph.SortByPhase();
    auto it = phaseGroups.find(phase);
    if (it == phaseGroups.end()) return;

    const auto& taskIds = it->second;
    std::unordered_map<TaskId, tf::Task> flowTasks;

    // 创建 tf::Task
    for (TaskId id : taskIds) {
        const Task* task = graph.GetTask(id);
        if (!task) continue;

        // Main/Render 线程任务：分发到专用队列
        if (task->thread == ThreadType::Main || task->thread == ThreadType::Render) {
            DispatchToThreadQueue(*task);
            continue;
        }

        // Any/Worker 任务
        tf::Task flowTask = m_taskflow.emplace([func = task->execute]() {
            func();
        });
        flowTask.name(task->name.c_str());
        flowTasks[id] = flowTask;
    }

    // 建立阶段内依赖
    for (TaskId id : taskIds) {
        const Task* task = graph.GetTask(id);
        if (!task || task->thread != ThreadType::Any) continue;

        auto flowTaskIt = flowTasks.find(id);
        if (flowTaskIt == flowTasks.end()) continue;

        for (TaskId depId : task->dependencies) {
            // 只建立同阶段内的依赖
            const Task* depTask = graph.GetTask(depId);
            if (!depTask || depTask->phase != phase) continue;

            auto depIt = flowTasks.find(depId);
            if (depIt != flowTasks.end()) {
                flowTaskIt->second.succeed(depIt->second);
            }
        }
    }

    // 执行本阶段任务
    if (!m_taskflow.empty()) {
        m_executor.run(m_taskflow).wait();
    }
}

void TaskExecutor::WaitForCompletion() {
    // TaskFlow 的 wait() 已经在 run() 中调用
    // 这里可以添加额外的同步逻辑
}

std::vector<std::function<void()>> TaskExecutor::StealMainThreadTasks() {
    std::lock_guard<std::mutex> lock(m_mainThreadMutex);
    return std::move(m_mainThreadQueue);
}

std::vector<std::function<void()>> TaskExecutor::StealRenderThreadTasks() {
    std::lock_guard<std::mutex> lock(m_renderThreadMutex);
    return std::move(m_renderThreadQueue);
}

size_t TaskExecutor::GetDynamicTaskCap() const {
    // 【预留插口】Frame Slicing 实现
    // 目前无限制，未来可根据帧率动态调整
    return 0;  // 0 = 无限制
}

void TaskExecutor::DispatchToThreadQueue(const Task& task) {
    switch (task.thread) {
        case ThreadType::Main: {
            std::lock_guard<std::mutex> lock(m_mainThreadMutex);
            m_mainThreadQueue.push_back(task.execute);
            break;
        }
        case ThreadType::Render: {
            std::lock_guard<std::mutex> lock(m_renderThreadMutex);
            m_renderThreadQueue.push_back(task.execute);
            break;
        }
        default:
            assert(false && "Invalid thread type for DispatchToThreadQueue");
    }
}

} // namespace DX12::Scheduler
