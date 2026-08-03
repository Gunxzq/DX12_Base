#include "TaskGraph.h"
#include <algorithm>
#include <stdexcept>

namespace DX12Engine::Scheduler {

TaskId TaskGraph::AddTask(Task task) {
    TaskId id = m_nextId++;
    task.id = id;
    m_tasks[id] = Node{std::move(task), {}, {}};
    return id;
}

void TaskGraph::AddDependency(TaskId task, TaskId dependency) {
    if (task == dependency) {
        throw std::invalid_argument("Task cannot depend on itself");
    }
    if (m_tasks.find(task) == m_tasks.end() || m_tasks.find(dependency) == m_tasks.end()) {
        throw std::invalid_argument("Invalid task ID");
    }

    m_tasks[task].dependencies.insert(dependency);
    m_tasks[dependency].dependents.insert(task);
}

void TaskGraph::AddDependencies(TaskId task, const std::vector<TaskId> &dependencies) {
    for (TaskId dep : dependencies) {
        AddDependency(task, dep);
    }
}

std::vector<TaskId> TaskGraph::TopologicalSort() const {
    std::vector<TaskId> result;
    result.reserve(m_tasks.size());

    auto inDegree = CalculateInDegree();
    std::queue<TaskId> queue;

    // 找到所有入度为 0 的节点
    for (const auto &[id, node] : m_tasks) {
        if (inDegree[id] == 0) {
            queue.push(id);
        }
    }

    // Kahn 算法
    while (!queue.empty()) {
        TaskId current = queue.front();
        queue.pop();
        result.push_back(current);

        const auto &node = m_tasks.at(current);
        for (TaskId dependent : node.dependents) {
            if (--inDegree[dependent] == 0) {
                queue.push(dependent);
            }
        }
    }

    // 检查是否有环
    if (result.size() != m_tasks.size()) {
        throw std::runtime_error("Cycle detected in task graph");
    }

    return result;
}

std::unordered_map<TaskPhase, std::vector<TaskId>> TaskGraph::SortByPhase() const {
    std::unordered_map<TaskPhase, std::vector<TaskId>> result;

    // 先拓扑排序
    auto sorted = TopologicalSort();

    // 按阶段分组
    for (TaskId id : sorted) {
        const auto &task = m_tasks.at(id).task;
        result[task.phase].push_back(id);
    }

    // 每个阶段内按优先级排序
    for (auto &[phase, ids] : result) {
        std::sort(ids.begin(), ids.end(),
                  [this](TaskId a, TaskId b) { return m_tasks.at(a).task.priority < m_tasks.at(b).task.priority; });
    }

    return result;
}

void TaskGraph::Clear() {
    m_tasks.clear();
    m_nextId = 1;
}

const Task *TaskGraph::GetTask(TaskId id) const {
    auto it = m_tasks.find(id);
    return (it != m_tasks.end()) ? &it->second.task : nullptr;
}

Task *TaskGraph::GetTask(TaskId id) {
    auto it = m_tasks.find(id);
    return (it != m_tasks.end()) ? &it->second.task : nullptr;
}

const std::unordered_set<TaskId> &TaskGraph::GetDependencies(TaskId id) const {
    // 依赖存储于 Node.dependencies（AddDependency 写入处），Task 结构本身不持有依赖字段
    static const std::unordered_set<TaskId> kEmpty;
    auto it = m_tasks.find(id);
    return (it != m_tasks.end()) ? it->second.dependencies : kEmpty;
}

bool TaskGraph::HasCycle() const { return !GetCyclePath().empty(); }

std::vector<TaskId> TaskGraph::GetCyclePath() const {
    // DFS 检测环并记录路径
    enum class Color { White, Gray, Black };
    std::unordered_map<TaskId, Color> color;
    std::unordered_map<TaskId, TaskId> parent;
    std::vector<TaskId> path;

    // 初始化颜色
    for (const auto &[id, _] : m_tasks) {
        color[id] = Color::White;
    }

    std::function<bool(TaskId)> dfs = [&](TaskId u) -> bool {
        color[u] = Color::Gray;

        const auto &node = m_tasks.at(u);
        for (TaskId v : node.dependents) { // 检查所有依赖 u 的节点
            if (color[v] == Color::Gray) {
                // 发现回边，存在环
                // 重建路径
                path.push_back(v);
                TaskId cur = u;
                while (cur != v) {
                    path.push_back(cur);
                    cur = parent[cur];
                }
                path.push_back(v);
                std::reverse(path.begin(), path.end());
                return true;
            }
            if (color[v] == Color::White) {
                parent[v] = u;
                if (dfs(v))
                    return true;
            }
        }

        color[u] = Color::Black;
        return false;
    };

    for (const auto &[id, _] : m_tasks) {
        if (color[id] == Color::White) {
            if (dfs(id))
                return path;
        }
    }

    return path; // 空路径表示无环
}

void TaskGraph::Validate() const {
    // 1. 检查循环依赖
    auto cycle = GetCyclePath();
    if (!cycle.empty()) {
        std::string msg = "Cycle detected in task graph: ";
        for (size_t i = 0; i < cycle.size(); ++i) {
            if (i > 0)
                msg += " -> ";
            const Task *task = GetTask(cycle[i]);
            msg += task ? task->name : std::to_string(cycle[i]);
        }
        throw std::runtime_error(msg);
    }

    // 2. 检查依赖是否存在
    for (const auto &[id, node] : m_tasks) {
        for (TaskId depId : node.dependencies) {
            if (m_tasks.find(depId) == m_tasks.end()) {
                const Task *task = GetTask(id);
                throw std::runtime_error("Task '" + (task ? task->name : std::to_string(id)) +
                                         "' depends on non-existent task: " + std::to_string(depId));
            }
        }
    }
}

std::unordered_map<TaskId, int> TaskGraph::CalculateInDegree() const {
    std::unordered_map<TaskId, int> inDegree;
    for (const auto &[id, node] : m_tasks) {
        inDegree[id] = static_cast<int>(node.dependencies.size());
    }
    return inDegree;
}

std::vector<Task> TaskGraph::GetTasks() const {
    std::vector<Task> result;
    result.reserve(m_tasks.size());

    // 按拓扑排序获取
    auto sortedIds = TopologicalSort();
    for (TaskId id : sortedIds) {
        auto it = m_tasks.find(id);
        if (it != m_tasks.end()) {
            result.push_back(it->second.task);
        }
    }

    return result;
}

} // namespace DX12Engine::Scheduler
