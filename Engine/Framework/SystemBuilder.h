#pragma once

/**
 * @file SystemBuilder.h
 * @brief System 构建器 - 流式API
 */

#include "SystemRegistry.h"

namespace DX12Engine::Scheduler {

/**
 * @brief System构建器 - 流式API
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
class SystemBuilder {
public:
    SystemBuilder(const std::string &name, TaskPhase phase, ThreadType thread);

    /// 设置执行函数
    SystemBuilder &Func(SystemFunc func);

    /// 添加感兴趣的消息类型
    template <typename EventType> SystemBuilder &WithMessage();

    /// 添加依赖的System
    SystemBuilder &DependsOn(const std::string &systemName);

    /// 设置优先级
    SystemBuilder &Priority(TaskPriority priority);

    /// 标记为常驻 System（每帧执行）
    SystemBuilder &AlwaysRun();

    SystemBuilder &RenderPhase(RenderPhase phase) {
        m_info.renderPhase = phase;
        return *this;
    }

    /// 完成注册
    SystemId Build();

private:
    SystemInfo m_info;
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
#define REGISTER_SYSTEM(Name, Phase, Thread)                                                                           \
    DX12Engine::Scheduler::SystemBuilder(#Name, DX12Engine::Scheduler::TaskPhase::Phase,                               \
                                         DX12Engine::Scheduler::ThreadType::Thread)

// ========================================================================
// 模板实现
// ========================================================================

template <typename EventType> SystemBuilder &SystemBuilder::WithMessage() {
    m_info.interestedMessages.push_back(EventType::StaticTypeHash);
    return *this;
}

} // namespace DX12Engine::Scheduler
