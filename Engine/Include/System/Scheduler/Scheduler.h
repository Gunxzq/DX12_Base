#pragma once

/**
 * @file Scheduler.h
 * @brief L3 调度层总入口
 *
 * L3 调度层职责（消息驱动版）：
 * 1. 消息收集：从 L1 通信层收集消息
 * 2. 动态构建：根据消息类型，动态构建本帧的 DAG
 * 3. 任务执行：线程池调度，任务分发到正确线程
 * 4. 帧同步：在正确时机触发 L4 层回调
 *
 * 使用示例（L4 层）：
 * @code
 * // 1. 初始化
 * DX12::ECS::Registry registry;
 * DX12::Scheduler::FrameDriver driver(registry);
 * driver.Initialize();
 *
 * // 2. 注册 System（L4 层）- 声明对哪些消息感兴趣
 * REGISTER_SYSTEM(PlayerMoveSystem, Update, Any)
 *     .WithMessage<PlayerInputEvent>()
 *     .DependsOn("PhysicsSystem")
 *     .Func([](Registry& r, const MessageContext& ctx) {
 *         // 处理玩家移动...
 *     });
 *
 * // 3. 发送消息（触发System执行）
 * PostEvent<PlayerInputEvent>({ .key = KeyCode::W });
 *
 * // 4. 运行
 * driver.Run();  // 阻塞主循环
 * @endcode
 */

#include "FrameDriver.h"
#include "Task.h"
#include "TaskExecutor.h"
#include "TaskGraph.h"
#include "TaskGraphBuilder.h"

// ========================================================================
// L4 层便捷 API
// ========================================================================

namespace DX12::Scheduler {

/**
 * @brief 注册一个 System 到调度器
 * @param name System 名称
 * @param phase 执行阶段
 * @param func System 函数
 * @return TaskId 任务 ID（用于声明依赖）
 */
inline TaskId RegisterSystem(const std::string &name, TaskPhase phase, std::function<void()> func) {
    auto &ctx = GetSchedulerContext();
    if (!ctx.taskGraph) {
        throw std::runtime_error("Scheduler context not initialized");
    }
    return ctx.taskGraph->AddTask(TaskFactory::Create(name, phase, std::move(func)));
}

/**
 * @brief 声明 System 依赖
 * @param task 依赖者
 * @param dependency 被依赖的任务
 */
inline void DependsOn(TaskId task, TaskId dependency) {
    auto &ctx = GetSchedulerContext();
    if (ctx.taskGraph) {
        ctx.taskGraph->AddDependency(task, dependency);
    }
}

/**
 * @brief 声明多依赖
 */
inline void DependsOn(TaskId task, const std::vector<TaskId> &dependencies) {
    auto &ctx = GetSchedulerContext();
    if (ctx.taskGraph) {
        ctx.taskGraph->AddDependencies(task, dependencies);
    }
}

/**
 * @brief 获取 Registry 引用
 */
inline ECS::Registry &Registry() {
    auto &ctx = GetSchedulerContext();
    if (!ctx.registry) {
        throw std::runtime_error("Registry not available in scheduler context");
    }
    return *ctx.registry;
}

/**
 * @brief 注册帧同步回调（L4 层多缓冲交换钩子）
 * @param callback 回调函数
 * @param name 回调名称（用于调试）
 * @return 回调 ID
 */
inline uint32_t OnFrameSync(std::function<void()> callback, const std::string &name = "") {
    auto &ctx = GetSchedulerContext();
    if (!ctx.frameDriver) {
        throw std::runtime_error("FrameDriver not available in scheduler context");
    }
    return ctx.frameDriver->RegisterFrameSyncCallback(std::move(callback), name);
}

/**
 * @brief 获取当前帧 DeltaTime
 */
inline float DeltaTime() {
    auto &ctx = GetSchedulerContext();
    return ctx.stats ? ctx.stats->deltaTime : 0.016f;
}

/**
 * @brief 获取当前帧号
 */
inline uint32_t FrameNumber() {
    auto &ctx = GetSchedulerContext();
    return ctx.stats ? ctx.stats->frameNumber : 0;
}

// ========================================================================
// 消息驱动 API（新增）
// ========================================================================

/**
 * @brief 发送事件到消息总线（不安全版本 - 仅用于演示）
 *
 * ⚠️ 警告：此版本使用栈上事件的指针。事件必须在帧末之前保持有效！
 *
 * 对于小型事件（<=32字节），建议使用 PostEventSmall，它会复制数据到Arena。
 * 对于大型事件，建议使用 PostEventPtr 配合堆分配或对象池。
 *
 * @tparam EventType 事件类型
 * @param event 事件数据引用（必须保持有效直到帧末！）
 * @param priority 事件优先级（决定入哪个桶）
 * @param senderId 发送者实体ID
 * @return true 发送成功，false 失败（Arena满）
 *
 * 使用示例（危险）：
 * @code
 * // ❌ 错误：事件在函数返回后销毁
 * void SendEvent() {
 *     PlayerInputEvent event{ .key = KeyCode::Space };
 *     PostEvent(event);  // 悬垂指针！
 * }
 *
 * // ✅ 正确：使用静态或堆分配
 * static PlayerInputEvent s_event;  // 静态生命周期
 * void SendEvent() {
 *     s_event.key = KeyCode::Space;
 *     PostEvent(s_event);
 * }
 * @endcode
 */
template <typename EventType>
inline bool
PostEvent(const EventType &event,
          DX12Engine::System::Event::EventPriority priority = DX12Engine::System::Event::EventPriority::P2_Normal,
          uint32_t senderId = 0) {
    auto &ctx = GetSchedulerContext();
    if (!ctx.frameDriver) {
        return false;
    }

    auto &arena = ctx.frameDriver->GetMessageArena();
    auto &bucketManager = ctx.frameDriver->GetBucketManager();

    // 分配槽位
    auto index = arena.AllocateSlot();
    if (index == DX12Engine::System::Event::MessageArena::INVALID_INDEX) {
        return false; // Arena满
    }

    // 写入消息（存储指针，调用方必须确保事件生命周期！）
    // 关键：Arena只存指针，不复制数据。事件必须在帧末之前保持有效。
    arena.WriteMessage<EventType>(index, senderId, const_cast<EventType *>(&event));

    // 入桶
    return bucketManager.PushMessage(index, priority);
}

/**
 * @brief 注册System（流式API）
 *
 * 使用示例：
 * @code
 * REGISTER_SYSTEM(PlayerMoveSystem, Update, Any)
 *     .WithMessage<PlayerInputEvent>()
 *     .DependsOn("PhysicsSystem")
 *     .Func([](Registry& r, const MessageContext& ctx) {
 *         // 处理逻辑
 *     });
 * @endcode
 */
#define REGISTER_SYSTEM(Name, Phase, Thread) DX12::Scheduler::SystemBuilder(#Name, TaskPhase::Phase, ThreadType::Thread)

/**
 * @brief 获取System注册表（用于查询已注册的System）
 */
inline const std::unordered_map<SystemId, SystemInfo> &GetSystemRegistry() { return SystemRegistry::GetAllSystems(); }

} // namespace DX12::Scheduler

// ========================================================================
// 静态分片 (Sharding) - 利用多核并行处理 Entity
// ========================================================================

namespace DX12::Scheduler {

/**
 * @brief 分片执行函数模板
 *
 * 将任务按分片数量并行执行。这是消除锁、利用多核的关键技术。
 *
 * @tparam Func 执行函数类型
 * @param shardCount 分片数量（通常 = 硬件线程数）
 * @param func 执行函数，参数为 shardIndex, shardCount
 *
 * 使用示例：
 * @code
 * // 在 System 中使用分片
 * ShardedExecute(8, [&, registry](size_t shardIdx, size_t shardCount) {
 *     for (auto [e, tf] : registry.view<Transform>()) {
 *         // 按 Entity ID 哈希分片
 *         if (std::hash<entt::entity>{}(e) % shardCount == shardIdx) {
 *             // 处理这个 Entity
 *         }
 *     }
 * });
 * @endcode
 */
template <typename Func> void ShardedExecute(size_t shardCount, Func &&func) {
    std::vector<std::future<void>> futures;
    futures.reserve(shardCount);

    for (size_t i = 0; i < shardCount; ++i) {
        futures.push_back(std::async(std::launch::async, [&, i, shardCount]() { func(i, shardCount); }));
    }

    for (auto &f : futures) {
        f.wait();
    }
}

/**
 * @brief 基于范围的简单分片
 *
 * 将 [begin, end) 范围均分为 shardCount 份，并行执行。
 *
 * @tparam Index 索引类型（通常是 size_t 或 int）
 * @tparam Func 执行函数类型
 * @param begin 起始索引
 * @param end 结束索引（不包含）
 * @param shardCount 分片数量
 * @param func 执行函数，参数为 shardBegin, shardEnd
 */
template <typename Index, typename Func> void RangeShard(Index begin, Index end, size_t shardCount, Func &&func) {
    Index total = end - begin;
    Index perShard = (total + shardCount - 1) / shardCount;

    std::vector<std::future<void>> futures;
    futures.reserve(shardCount);

    for (size_t i = 0; i < shardCount; ++i) {
        Index shardBegin = begin + static_cast<Index>(i * perShard);
        Index shardEnd = std::min(shardBegin + perShard, end);

        if (shardBegin >= end)
            break;

        futures.push_back(
            std::async(std::launch::async, [func, shardBegin, shardEnd]() { func(shardBegin, shardEnd); }));
    }

    for (auto &f : futures) {
        f.wait();
    }
}

} // namespace DX12::Scheduler

// ========================================================================
// 便捷宏（可选）
// ========================================================================

/**
 * @brief 定义 System 并自动注册到当前调度器
 *
 * 使用示例：
 * @code
 * DEFINE_SYSTEM(PlayerController, Update) {
 *     // 每帧执行的逻辑
 *     auto& registry = DX12::Scheduler::Registry();
 *     for (auto [entity, input, transform] : registry.view<Input, Transform>()) {
 *         // 处理输入...
 *     }
 * }
 * @endcode
 */
#define DEFINE_SYSTEM(Name, Phase)                                                                                     \
    static struct Name##SystemRegistrar {                                                                              \
        Name##SystemRegistrar() {                                                                                      \
            DX12::Scheduler::RegisterSystem(#Name, DX12::Scheduler::TaskPhase::Phase, Name##System::Execute);          \
        }                                                                                                              \
    } g_##Name##SystemRegistrar;                                                                                       \
    struct Name##System {                                                                                              \
        static void Execute();                                                                                         \
    };                                                                                                                 \
    void Name##System::Execute()

/**
 * @brief 声明 System 依赖
 * @code
 * DEFINE_SYSTEM(PlayerRender, Render);
 * DEFINE_SYSTEM(PlayerUpdate, Update);
 * SYSTEM_DEPENDS_ON(PlayerRender, PlayerUpdate);  // 渲染依赖更新
 * @endcode
 */
#define SYSTEM_DEPENDS_ON(Task, Dependency)                                                                            \
    static struct Task##DependsOn##Dependency {                                                                        \
        Task##DependsOn##Dependency() { DX12::Scheduler::DependsOn(Task, Dependency); }                                \
    } g_##Task##DependsOn##Dependency

/**
 * @brief 定义分片 System（自动并行化）
 *
 * 使用示例：
 * @code
 * DEFINE_SHARDED_SYSTEM(ParticleUpdate, Update, 8) {
 *     // 这个函数会被调用 8 次，每次处理 1/8 的 Entity
 *     // shardIdx: 当前分片索引 (0-7)
 *     // shardCount: 总分片数 (8)
 *     auto& registry = DX12::Scheduler::Registry();
 *     for (auto [e, particle] : registry.view<Particle>()) {
 *         if (std::hash<entt::entity>{}(e) % shardCount == shardIdx) {
 *             // 更新粒子
 *         }
 *     }
 * }
 * @endcode
 */
#define DEFINE_SHARDED_SYSTEM(Name, Phase, ShardCount)                                                                 \
    static struct Name##SystemRegistrar {                                                                              \
        Name##SystemRegistrar() {                                                                                      \
            DX12::Scheduler::RegisterSystem(#Name, DX12::Scheduler::TaskPhase::Phase, Name##System::Execute);          \
        }                                                                                                              \
    } g_##Name##SystemRegistrar;                                                                                       \
    struct Name##System {                                                                                              \
        static void Execute() { DX12::Scheduler::ShardedExecute(ShardCount, Name##System::ExecuteShard); }             \
        static void ExecuteShard(size_t shardIdx, size_t shardCount);                                                  \
    };                                                                                                                 \
    void Name##System::ExecuteShard(size_t shardIdx, size_t shardCount)
