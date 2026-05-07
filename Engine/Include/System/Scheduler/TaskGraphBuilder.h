#pragma once

#include "System/Scheduler/TaskGraph.h"  // 已包含Task.h
#include "System/ECS/Registry.h"
#include "System/Event/Event.h"
#include "System/Event/BucketManager.h"
#include "System/Event/MessageArena.h"
#include <functional>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <string>

namespace DX12::Scheduler {

// ========================================================================
// 前置声明
// ========================================================================

class TaskGraph;
class FrameDriver;

// ========================================================================
// 类型定义
// ========================================================================

using SystemId = uint32_t;
using MessageTypeHash = DX12Engine::System::Event::EventTypeHash;

// ========================================================================
// 消息上下文 - 传递给Task的执行上下文
// ========================================================================

/**
 * @brief 消息执行上下文
 * 
 * 当Task被消息触发执行时，包含触发它的消息信息
 */
struct MessageContext {
    MessageTypeHash messageType = 0;
    uint32_t senderId = 0;
    void* payload = nullptr;
    float deltaTime = 0.0f;
    uint32_t frameNumber = 0;
};

// ========================================================================
// System 定义
// ========================================================================

/**
 * @brief System函数类型
 * 
 * L4层逻辑函数的标准签名
 */
using SystemFunc = std::function<void(ECS::Registry&, const MessageContext&)>;

/**
 * @brief System元数据
 */
struct SystemInfo {
    SystemId id = 0;
    std::string name;
    SystemFunc func;
    TaskPhase phase = TaskPhase::Update;
    ThreadType threadType = ThreadType::Any;
    TaskPriority priority = TaskPriority::Normal;
    std::vector<SystemId> dependencies;  // 依赖的其他System
    std::vector<MessageTypeHash> interestedMessages;  // 感兴趣的消息类型
};

// ========================================================================
// System注册表 - L4层在此注册System
// ========================================================================

/**
 * @brief System注册表
 * 
 * L4层在初始化时注册所有System，建立消息到System的映射
 * 
 * 使用示例：
 * @code
 * // L4层注册System
 * SystemRegistry::Register({
 *     .name = "PlayerMoveSystem",
 *     .func = [](Registry& r, const MessageContext& ctx) {
 *         // 处理玩家移动逻辑
 *     },
 *     .phase = TaskPhase::Update,
 *     .interestedMessages = { PlayerInputEvent::StaticTypeHash }
 * });
 * @endcode
 */
class SystemRegistry {
public:
    /// 注册一个System
    static SystemId Register(SystemInfo info);
    
    /// 根据ID获取System信息
    static const SystemInfo* GetSystem(SystemId id);
    
    /// 根据名称获取System信息
    static const SystemInfo* GetSystemByName(const std::string& name);
    
    /// 获取对某消息感兴趣的所有System
    static std::vector<SystemId> GetInterestedSystems(MessageTypeHash messageType);
    
    /// 获取所有已注册的System
    static const std::unordered_map<SystemId, SystemInfo>& GetAllSystems();
    
    /// 清空所有注册
    static void Clear();

private:
    static SystemId s_nextId;
    static std::unordered_map<SystemId, SystemInfo> s_systems;
    static std::unordered_map<std::string, SystemId> s_nameToId;
    static std::unordered_map<MessageTypeHash, std::vector<SystemId>> s_messageToSystems;
};

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
     * @param bucketManager 消息桶管理器
     * @param arena 消息Arena
     * @param registry ECS注册表
     * @param frameStats 当前帧统计信息
     */
    static void BuildFromBuckets(
        TaskGraph& graph,
        DX12Engine::System::Event::BucketManager& bucketManager,
        DX12Engine::System::Event::MessageArena& arena,
        ECS::Registry& registry,
        const struct FrameStats& frameStats
    );

    /**
     * @brief 收集所有待处理的消息
     * 
     * 从所有优先级桶中窃取消息
     */
    static std::vector<MessageContext> CollectMessages(
        DX12Engine::System::Event::BucketManager& bucketManager,
        DX12Engine::System::Event::MessageArena& arena
    );

private:
    /**
     * @brief 根据消息激活对应的System
     */
    static std::unordered_set<SystemId> ActivateSystems(
        const std::vector<MessageContext>& messages
    );

    /**
     * @brief 将激活的System转化为Task并添加到图中
     */
    static std::unordered_map<SystemId, TaskId> BuildTasks(
        TaskGraph& graph,
        const std::unordered_set<SystemId>& activatedSystems,
        const FrameStats& frameStats
    );

    /**
     * @brief 建立Task之间的依赖关系
     */
    static void BuildDependencies(
        TaskGraph& graph,
        const std::unordered_set<SystemId>& activatedSystems,
        const std::unordered_map<SystemId, TaskId>& systemToTask
    );
};

// ========================================================================
// 便捷宏 - 简化System注册
// ========================================================================

/**
 * @brief 注册System的便捷宏
 * 
 * 使用示例：
 * @code
 * REGISTER_SYSTEM(PlayerMoveSystem, Update, Any)
 *     .WithMessage<PlayerInputEvent>()
 *     .DependsOn("PhysicsSystem")
 *     .Func([](Registry& r, const MessageContext& ctx) {
 *         // 逻辑代码
 *     });
 * @endcode
 */
#define REGISTER_SYSTEM(Name, Phase, Thread) \
    DX12::Scheduler::SystemBuilder(#Name, TaskPhase::Phase, ThreadType::Thread)

/**
 * @brief System构建器 - 流式API
 */
class SystemBuilder {
public:
    SystemBuilder(const std::string& name, TaskPhase phase, ThreadType thread);
    
    /// 设置执行函数
    SystemBuilder& Func(SystemFunc func);
    
    /// 添加感兴趣的消息类型
    template<typename EventType>
    SystemBuilder& WithMessage();
    
    /// 添加依赖的System
    SystemBuilder& DependsOn(const std::string& systemName);
    
    /// 设置优先级
    SystemBuilder& Priority(TaskPriority priority);
    
    /// 完成注册
    SystemId Build();

private:
    SystemInfo m_info;
};

template<typename EventType>
SystemBuilder& SystemBuilder::WithMessage() {
    m_info.interestedMessages.push_back(EventType::StaticTypeHash);
    return *this;
}

} // namespace DX12::Scheduler
