#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace DX12Engine {
namespace Scheduler {

// ========================================================================
// 任务标识与类型
// ========================================================================

using TaskId = uint32_t;
inline constexpr TaskId INVALID_TASK_ID = 0;

/// 任务执行阶段（用于确定任务在帧中的执行时机）
enum class TaskPhase : uint8_t {
    EarlyUpdate, // 最先执行：输入处理、网络接收
    Update,      // 主逻辑更新：Gameplay、Physics
    LateUpdate,  // 后处理：动画、Transform 计算
    PreCulling,  // 剔除准备：视锥剔除 + LOD 计算（拆分自 PreRender）
    PreRender,   // 渲染准备：构建器并行生成渲染队列
    Render,      // 渲染提交（单线程）
    PostRender,  // 渲染后处理
    Count
};

/// 任务执行线程类型
enum class ThreadType : uint8_t {
    Any,    // 任意工作线程
    Main,   // 主线程（逻辑线程）
    Render, // 渲染线程
    Worker  // 工作线程池
};

/// 任务优先级
enum class TaskPriority : uint8_t {
    Critical = 0,  // 关键任务
    High = 1,      // 高优先级
    Normal = 2,    // 普通优先级
    Low = 3,       // 低优先级
    Background = 4 // 后台任务
};

// ========================================================================
// 任务定义
// ========================================================================

/**
 * @brief L4 层提交的任务单元
 *
 * L4 的 System 被包装成 Task 提交给 L3 调度器。
 * Task 是 L3 调度的最小单位。
 */
struct Task {
    TaskId id = INVALID_TASK_ID;
    std::string name;
    TaskPhase phase = TaskPhase::Update;
    ThreadType thread = ThreadType::Any;
    std::function<void()> execute;
    std::vector<TaskId> dependencies; // 依赖的任务 ID
    uint32_t priority = 100;          // 优先级（数字越小越优先）

    bool operator<(const Task &other) const {
        return priority > other.priority; // 优先级队列：小数字先出
    }
};

/// 任务工厂：方便 L4 层创建任务
class TaskFactory {
public:
    /// 创建简单任务
    static Task Create(const std::string &name, TaskPhase phase, std::function<void()> func) {
        Task task;
        task.name = name;
        task.phase = phase;
        task.execute = std::move(func);
        return task;
    }

    /// 创建主线程任务（必须在主线程执行）
    static Task CreateMainThread(const std::string &name, std::function<void()> func) {
        Task task;
        task.name = name;
        task.phase = TaskPhase::Update;
        task.thread = ThreadType::Main;
        task.execute = std::move(func);
        return task;
    }

    /// 创建渲染任务（必须在渲染线程执行）
    static Task CreateRenderThread(const std::string &name, std::function<void()> func) {
        Task task;
        task.name = name;
        task.phase = TaskPhase::Render;
        task.thread = ThreadType::Render;
        task.execute = std::move(func);
        return task;
    }
};
} // namespace Scheduler

} // namespace DX12Engine
