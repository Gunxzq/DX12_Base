#include "System/Scheduler/TaskGraphBuilder.h"
#include "System/Scheduler/FrameDriver.h"
#include <algorithm>

namespace DX12::Scheduler {

// ========================================================================
// SystemRegistry 实现
// ========================================================================

SystemId SystemRegistry::s_nextId = 1;
std::unordered_map<SystemId, SystemInfo> SystemRegistry::s_systems;
std::unordered_map<std::string, SystemId> SystemRegistry::s_nameToId;
std::unordered_map<MessageTypeHash, std::vector<SystemId>> SystemRegistry::s_messageToSystems;

SystemId SystemRegistry::Register(SystemInfo info) {
    SystemId id = s_nextId++;
    info.id = id;

    s_systems[id] = std::move(info);
    s_nameToId[s_systems[id].name] = id;

    // 建立消息到System的映射
    for (auto msgType : s_systems[id].interestedMessages) {
        s_messageToSystems[msgType].push_back(id);
    }

    return id;
}

const SystemInfo *SystemRegistry::GetSystem(SystemId id) {
    auto it = s_systems.find(id);
    return (it != s_systems.end()) ? &it->second : nullptr;
}

const SystemInfo *SystemRegistry::GetSystemByName(const std::string &name) {
    auto it = s_nameToId.find(name);
    if (it != s_nameToId.end()) {
        return GetSystem(it->second);
    }
    return nullptr;
}

std::vector<SystemId> SystemRegistry::GetInterestedSystems(MessageTypeHash messageType) {
    auto it = s_messageToSystems.find(messageType);
    if (it != s_messageToSystems.end()) {
        return it->second;
    }
    return {};
}

const std::unordered_map<SystemId, SystemInfo> &SystemRegistry::GetAllSystems() { return s_systems; }

void SystemRegistry::Clear() {
    s_systems.clear();
    s_nameToId.clear();
    s_messageToSystems.clear();
    s_nextId = 1;
}

// ========================================================================
// SystemBuilder 实现
// ========================================================================

SystemBuilder::SystemBuilder(const std::string &name, TaskPhase phase, ThreadType thread) {
    m_info.name = name;
    m_info.phase = phase;
    m_info.threadType = thread;
}

SystemBuilder &SystemBuilder::Func(SystemFunc func) {
    m_info.func = std::move(func);
    return *this;
}

SystemBuilder &SystemBuilder::DependsOn(const std::string &systemName) {
    const auto *depSystem = SystemRegistry::GetSystemByName(systemName);
    if (depSystem) {
        m_info.dependencies.push_back(depSystem->id);
    }
    return *this;
}

SystemBuilder &SystemBuilder::Priority(TaskPriority priority) {
    m_info.priority = priority;
    return *this;
}

SystemId SystemBuilder::Build() { return SystemRegistry::Register(std::move(m_info)); }

// ========================================================================
// TaskGraphBuilder 实现
// ========================================================================

void TaskGraphBuilder::BuildFromBuckets(TaskGraph &graph, DX12Engine::System::Event::BucketManager &bucketManager,
                                        DX12Engine::System::Event::MessageArena &arena, ECS::Registry &registry,
                                        const FrameStats &frameStats) {
    // ========================================================================
    // 阶段 0: 清空上一帧的图
    // ========================================================================
    graph.Clear();

    // ========================================================================
    // 阶段 1: 收集所有消息
    // ========================================================================
    auto messages = CollectMessages(bucketManager, arena);

    // 如果没有消息，且没有常驻System，图保持为空
    // 空的图在执行阶段会直接跳过，实现"极致节能"
    if (messages.empty()) {
        return;
    }

    // ========================================================================
    // 阶段 2: 根据消息激活对应的System
    // ========================================================================
    auto activatedSystems = ActivateSystems(messages);

    // ========================================================================
    // 阶段 3: 将激活的System转化为Task
    // ========================================================================
    auto systemToTask = BuildTasks(graph, activatedSystems, frameStats);

    // ========================================================================
    // 阶段 4: 建立依赖关系
    // ========================================================================
    BuildDependencies(graph, activatedSystems, systemToTask);

    // ========================================================================
    // 阶段 5: 验证图的合法性
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

std::vector<MessageContext> TaskGraphBuilder::CollectMessages(DX12Engine::System::Event::BucketManager &bucketManager,
                                                              DX12Engine::System::Event::MessageArena &arena) {
    std::vector<MessageContext> messages;

    // 预分配空间（优化：避免多次扩容）
    messages.reserve(256);

    DX12Engine::System::Event::MessageIndex index;
    DX12Engine::System::Event::EventPriority priority;

    // 从所有优先级桶中窃取消息
    // BucketManager会自动处理优先级和Aging
    while (bucketManager.PopNextMessage(index, priority)) {
        // 关键：先检查消息是否已提交（payload 非空）
        // GetPayload 使用 acquire 语义，确保看到完整的写入数据
        void* payload = arena.GetPayload(index);
        if (payload == nullptr) {
            // 消息尚未提交（极罕见情况：生产者正在写入）
            // 跳过此消息，它将在下一帧被处理
            continue;
        }

        MessageContext ctx;
        ctx.messageType = arena.GetType(index);
        ctx.senderId = arena.GetSender(index);
        ctx.payload = payload;

        messages.push_back(ctx);
    }

    return messages;
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

std::unordered_map<SystemId, TaskId> TaskGraphBuilder::BuildTasks(TaskGraph &graph,
                                                                  const std::unordered_set<SystemId> &activatedSystems,
                                                                  const FrameStats &frameStats) {
    std::unordered_map<SystemId, TaskId> systemToTask;

    for (SystemId sysId : activatedSystems) {
        const auto *info = SystemRegistry::GetSystem(sysId);
        if (!info)
            continue;

        // 创建Task
        Task task;
        task.name = info->name;
        task.phase = info->phase;
        task.thread = info->threadType;
        task.priority = static_cast<uint32_t>(info->priority);

        // 包装System函数，注入上下文
        // 注意：这里使用lambda捕获info指针，需要确保info生命周期
        task.execute = [info]() {
            MessageContext msgCtx;
            msgCtx.deltaTime = 0.016f; // 默认值
            msgCtx.frameNumber = 0;

            if (info->func) {
                // TODO: 需要传入registry和msgCtx
                // info->func(registry, msgCtx);
            }
        };

        TaskId tid = graph.AddTask(std::move(task));
        systemToTask[sysId] = tid;
    }

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

} // namespace DX12::Scheduler
