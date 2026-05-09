#pragma once

#include "System/ECS/Registry.h"
#include "System/Event/MessageDispatcher.h"
#include "System/Framework/SystemBuilder.h"
#include "System/Framework/SystemTypes.h"
#include "System/Scheduler/Task.h"
#include "System/Scheduler/TaskGraph.h"
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace DX12Engine::Scheduler {

// ========================================================================
// TaskGraphBuilder - 消息驱动的DAG构建器
// ========================================================================

// 前置声明
struct FrameStats;

/**
 * @brief TaskGraph构建器
 *
 * 核心职责：
 * 1. 从通信层收集消息
 * 2. 根据消息类型找到对应的System
 * 3. 将System包装成Task并建立依赖关系
 * 4. 构建出本帧需要执行的DAG
 *
 * 这是L1通信层与L3调度层之间的"粘合剂"
 */
class TaskGraphBuilder {
public:
    /**
     * @brief 从消息桶构建任务图
     *
     * 这是核心入口函数，每帧Tick时调用
     *
     * @param graph 输出的任务图（会被清空并重建）
     * @param dispatcher 消息分发器（Event层对外唯一接口）
     * @param registry ECS注册表
     * @param frameStats 当前帧统计信息
     */
    static void BuildFromBuckets(TaskGraph &graph, DX12Engine::System::Event::MessageDispatcher &dispatcher,
                                 ECS::Registry &registry, const struct FrameStats &frameStats);

private:
    /**
     * @brief 根据消息激活对应的System
     */
    static std::unordered_set<SystemId> ActivateSystems(const std::vector<MessageContext> &messages);

    /**
     * @brief 将激活的System转化为Task并添加到图中
     */
    static std::unordered_map<SystemId, TaskId> BuildTasks(TaskGraph &graph,
                                                           const std::unordered_set<SystemId> &activatedSystems,
                                                           const std::vector<MessageContext> &messages,
                                                           ECS::Registry &registry, const FrameStats &frameStats);

    /**
     * @brief 建立Task之间的依赖关系
     */
    static void BuildDependencies(TaskGraph &graph, const std::unordered_set<SystemId> &activatedSystems,
                                  const std::unordered_map<SystemId, TaskId> &systemToTask);
};

// ========================================================================
// 线程局部消息上下文访问接口
// ========================================================================

// 获取当前消息上下文（由 TaskGraphBuilder 设置，供 Task 执行时使用）
// 定义在 TaskGraphBuilder.cpp 中
const MessageContext *GetCurrentMessageContext();

} // namespace DX12Engine::Scheduler
