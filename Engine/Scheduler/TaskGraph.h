#pragma once
#include "Task.h"
#include <memory>
#include <queue>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace DX12Engine {
namespace Scheduler {

// ========================================================================
// 任务图 (DAG - 有向无环图)
// ========================================================================

/**
 * @brief 管理任务依赖关系的 DAG
 *
 * 职责：
 * 1. 接收 L4 提交的任务
 * 2. 根据依赖关系构建 DAG
 * 3. 拓扑排序生成执行序列
 *
 * 设计原则：
 * - 只负责"排序"，不负责"执行"
 * - 线程安全：所有操作在主线程完成（构建阶段）
 */
class TaskGraph {
public:
    TaskGraph() = default;
    ~TaskGraph() = default;

    // 禁止拷贝，允许移动
    TaskGraph(const TaskGraph &) = delete;
    TaskGraph &operator=(const TaskGraph &) = delete;
    TaskGraph(TaskGraph &&) noexcept = default;
    TaskGraph &operator=(TaskGraph &&) noexcept = default;

    /// 添加任务到图中
    TaskId AddTask(Task task);

    /// 声明任务依赖：task 依赖于 dependency
    void AddDependency(TaskId task, TaskId dependency);

    /// 批量声明依赖
    void AddDependencies(TaskId task, const std::vector<TaskId> &dependencies);

    /// 拓扑排序，生成可执行序列
    /// @return 按执行顺序排列的任务 ID 列表
    std::vector<TaskId> TopologicalSort() const;

    /// 按阶段分组排序
    std::unordered_map<TaskPhase, std::vector<TaskId>> SortByPhase() const;

    /// 清空所有任务
    void Clear();

    /// 获取任务（用于执行器查询）
    const Task *GetTask(TaskId id) const;
    Task *GetTask(TaskId id);

    /// 获取任务的依赖集合（我依赖谁）
    /// @param id 任务 ID
    /// @return 依赖任务 ID 集合（不存在返回空集）
    const std::unordered_set<TaskId> &GetDependencies(TaskId id) const;

    /// 检查是否存在循环依赖（DFS 算法）
    /// @return 如果存在循环依赖，返回 true
    bool HasCycle() const;

    /// 获取循环依赖路径（用于调试）
    /// @return 循环路径上的任务 ID 列表，无环时返回空
    std::vector<TaskId> GetCyclePath() const;

    /// 验证图的有效性（无环、所有依赖存在）
    /// @throws std::runtime_error 如果图无效
    void Validate() const;

    /// 获取任务数量
    size_t GetTaskCount() const { return m_tasks.size(); }

    /// 获取所有任务（按拓扑排序，用于遍历）
    std::vector<Task> GetTasks() const;

private:
    struct Node {
        Task task;
        std::unordered_set<TaskId> dependencies; // 我依赖谁
        std::unordered_set<TaskId> dependents;   // 谁依赖我
    };

    std::unordered_map<TaskId, Node> m_tasks;
    TaskId m_nextId = 1;

    // Kahn 算法辅助
    std::unordered_map<TaskId, int> CalculateInDegree() const;
};

} // namespace Scheduler
} // namespace DX12Engine
