#include "System/Scheduler/TaskGraphBuilder.h"
#include <algorithm>
#include <chrono>

namespace DX12Engine::Scheduler {

// ========================================================================
// 辅助函数
// ========================================================================

/**
 * @brief 获取当前时间戳（微秒）
 */
inline uint64_t GetCurrentTimeUs() {
    auto now = std::chrono::high_resolution_clock::now();
    return std::chrono::duration_cast<std::chrono::microseconds>(now.time_since_epoch()).count();
}

/**
 * @brief 从 Dispatcher 收集消息并构建 MessageContext
 *
 * 通信层只提供原始 MessageIndex，调度层负责：
 * 1. 调用 FlushEvents 获取索引
 * 2. 通过 GetArena 读取原始消息
 * 3. 构建完整的 MessageContext（填充 receiveTimestamp）
 */
std::vector<MessageContext> CollectMessages(DX12Engine::System::Event::MessageDispatcher &dispatcher,
                                            const FrameStats &frameStats) {
    std::vector<MessageContext> messages;

    // 预算：每帧最多处理 1024 条消息，时间预算 1ms
    DX12Engine::System::Event::FlushBudget budget;
    budget.hardLimit = 1024;
    budget.maxTimeUs = 1000; // 1ms

    std::vector<DX12Engine::System::Event::MessageIndex> indices;
    uint32_t count = dispatcher.FlushEvents(indices, budget);

    if (count == 0) {
        return messages;
    }

    messages.reserve(count);

    auto &arena = dispatcher.GetArena();
    uint64_t receiveTime = GetCurrentTimeUs();

    for (auto idx : indices) {
        auto content = arena.GetMessage(idx);

        MessageContext ctx;
        ctx.messageType = content.typeHash;
        ctx.senderId = content.senderId;
        ctx.payload = content.payload;
        ctx.sendTimestamp = content.sendTimestamp;
        ctx.receiveTimestamp = receiveTime;

        messages.push_back(ctx);
    }

    return messages;
}

// ========================================================================
// TaskGraphBuilder 实现
// ========================================================================

void TaskGraphBuilder::BuildFromBuckets(TaskGraph &graph, DX12Engine::System::Event::MessageDispatcher &dispatcher,
                                        ECS::Registry &registry, const FrameStats &frameStats) {
    // ========================================================================
    // 阶段 0: 清空上一帧的图
    // ========================================================================
    graph.Clear();

    // ========================================================================
    // 阶段 1: 收集所有消息（调度层自己构建 MessageContext）
    // ========================================================================
    auto messages = CollectMessages(dispatcher, frameStats);

    // 如果没有消息，且没有常驻System，图保持为空
    // 空的图在执行阶段会直接跳过，实现"极致节能"
    if (messages.empty()) {
        return;
    }

    // ========================================================================
    // 阶段 2: 将消息和System转化为Task
    // ========================================================================
    // BuildTasks 会：
    // 1. 遍历每条消息
    // 2. 找到对该消息感兴趣的 System
    // 3. 为每个 System 创建 Task（通过线程局部上下文传递消息）
    auto systemToTask = BuildTasks(graph, {}, messages, registry, frameStats);

    // ========================================================================
    // 阶段 3: 建立依赖关系（暂时为空，为未来扩展预留）
    // ========================================================================
    // BuildDependencies(graph, {}, systemToTask);

    // ========================================================================
    // 阶段 4: 验证图的合法性
    // ========================================================================
    try {
        graph.Validate();
    } catch (const std::runtime_error &e) {
        // TODO: 使用Logger记录错误
        // 如果验证失败，清空图避免执行错误任务
        graph.Clear();
        throw;
    }
}

std::unordered_set<SystemId> TaskGraphBuilder::ActivateSystems(const std::vector<MessageContext> &messages) {
    std::unordered_set<SystemId> activated;

    // 扫描每条消息，找到对它感兴趣的System
    for (const auto &msg : messages) {
        auto systems = SystemRegistry::GetInterestedSystems(msg.messageType);
        for (SystemId id : systems) {
            activated.insert(id);
        }
    }

    // 同时加入"常驻System"（每帧都运行的System，如渲染准备）
    // 这些System在注册时标记为"AlwaysRun"
    for (const auto &[id, info] : SystemRegistry::GetAllSystems()) {
        // TODO: 添加AlwaysRun标记支持
        // if (info.alwaysRun) {
        //     activated.insert(id);
        // }
    }

    return activated;
}

// 线程局部消息上下文（Task执行时通过 GetCurrentMessageContext() 访问）
thread_local const MessageContext *g_currentMessageContext = nullptr;
thread_local std::vector<MessageContext> g_currentMessages;

// 获取当前消息上下文
const MessageContext *GetCurrentMessageContext() { return g_currentMessageContext; }

std::unordered_map<SystemId, TaskId> TaskGraphBuilder::BuildTasks(TaskGraph &graph,
                                                                  const std::unordered_set<SystemId> &activatedSystems,
                                                                  const std::vector<MessageContext> &messages,
                                                                  ECS::Registry &registry,
                                                                  const FrameStats &frameStats) {
    std::unordered_map<SystemId, TaskId> systemToTask;

    // 保存到线程局部变量，供 GetCurrentMessageContext() 使用
    g_currentMessages = messages;

    // 遍历每条消息，为处理该消息的每个 System 创建 Task
    for (const auto &msgCtx : messages) {
        // 设置当前消息上下文
        g_currentMessageContext = &msgCtx;

        // 找到对该消息感兴趣的 System
        auto systems = SystemRegistry::GetInterestedSystems(msgCtx.messageType);
        for (SystemId sysId : systems) {
            // 避免重复创建 Task
            if (systemToTask.find(sysId) != systemToTask.end()) {
                continue;
            }

            const auto *info = SystemRegistry::GetSystem(sysId);
            if (!info)
                continue;

            // 创建Task
            Task task;
            task.name = info->name;
            task.phase = info->phase;
            task.thread = info->threadType;
            task.priority = static_cast<uint32_t>(info->priority);

            // 包装System函数，直接捕获 msgCtx（不依赖 thread-local，避免悬空指针）
            task.execute = [info, &registry, msgCtx]() {
                if (info->func) {
                    info->func(registry, msgCtx);
                }
            };

            TaskId tid = graph.AddTask(std::move(task));
            systemToTask[sysId] = tid;
        }
    }

    // 清理
    g_currentMessageContext = nullptr;

    return systemToTask;
}

void TaskGraphBuilder::BuildDependencies(TaskGraph &graph, const std::unordered_set<SystemId> &activatedSystems,
                                         const std::unordered_map<SystemId, TaskId> &systemToTask) {
    for (SystemId sysId : activatedSystems) {
        const auto *info = SystemRegistry::GetSystem(sysId);
        if (!info)
            continue;

        auto it = systemToTask.find(sysId);
        if (it == systemToTask.end())
            continue;

        TaskId taskId = it->second;

        // 为每个依赖建立边
        for (SystemId depId : info->dependencies) {
            auto depIt = systemToTask.find(depId);
            if (depIt != systemToTask.end()) {
                // 依赖的System也在本帧激活，建立边
                graph.AddDependency(taskId, depIt->second);
            }
            // 如果依赖的System没激活，说明它不需要运行
            // 数据应该是上一帧的结果，或者由L4层自己管理
        }
    }
}

} // namespace DX12Engine::Scheduler
