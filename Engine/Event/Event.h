#pragma once

#include <chrono>
#include <cstdint>
#include <type_traits>

namespace DX12Engine {

namespace Event {

// ========================================================================
// 1. 基础枚举与类型
// ========================================================================

/**
 * @brief 事件优先级
 * 对应 L1 通信层的优先级桶
 */
enum class EventPriority : uint8_t {
    P0_Critical = 0,  // SystemAlert: 内存溢出、强制退出
    P1_High = 1,      // Physics: 碰撞、触发器
    P2_Normal = 2,    // GameLogic: 扣血、技能
    P3_Low = 3,       // Render: 特效、UI
    P4_Background = 4 // Async: 资源加载结果
};

/**
 * @brief 事件类型哈希别名
 */
using EventTypeHash = uint32_t;

/**
 * @brief 消息在 Arena 中的索引类型
 */
using MessageIndex = uint32_t;

// ========================================================================
// 2. 辅助宏 (用于生成标准事件结构体)
// ========================================================================

/**
 * @brief 定义事件的标准头部字段
 *
 * 注意：在逻辑事件结构体中，这些字段主要用于初始化。
 * 在物理 Arena 中，它们会被拆分存储。
 */
#define EVENT_HEADER_FIELDS EventPriority Priority;

/**
 * @brief 初始化事件头部
 *
 * 在事件构造函数成员初始化列表中使用。
 * @param prio 优先级
 */
#define INIT_EVENT_HEADER(prio) Priority(prio)

/**
 * @brief 定义事件类型哈希
 *
 * 提供静态和实例方法获取类型哈希，用于 EventBus 路由。
 * @param HashValue 唯一的静态哈希值 (建议手动分配或使用 constexpr hash)
 */
#define DEFINE_EVENT_TYPE_HASH(HashValue)                                                                              \
    static constexpr EventTypeHash StaticTypeHash = HashValue;                                                         \
    inline EventTypeHash GetTypeHash() const { return StaticTypeHash; }                                                \
    inline static EventTypeHash GetStaticTypeHash() { return StaticTypeHash; }

} // namespace Event

} // namespace DX12Engine