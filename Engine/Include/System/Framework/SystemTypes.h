#pragma once

/**
 * @file SystemTypes.h
 * @brief Framework 层共享类型定义
 *
 * 定义 System 相关的核心类型，供 SystemRegistry 和 SystemBuilder 使用
 */

#include "System/ECS/Registry.h"
#include "System/Scheduler/RenderPhase.h"
#include "System/Scheduler/Task.h"
#include <functional>
#include <string>
#include <vector>

namespace DX12Engine::Scheduler {

// ========================================================================
// 类型别名
// ========================================================================

/// SystemID 是 TaskId 的别名，保持概念清晰
using SystemId = TaskId;
/// 消息类型哈希
using MessageTypeHash = uint32_t;

// ========================================================================
// 消息上下文 - 传递给Task的执行上下文
// ========================================================================

/**
 * @brief 消息执行上下文
 *
 * 当Task被消息触发执行时，包含触发它的消息信息。
 * Payload 遵循位布局约定，System 层使用辅助方法自行解析。
 */
struct MessageContext {
    // ========== 消息数据 ==========
    MessageTypeHash messageType = 0;
    uint32_t senderId = 0;
    uint64_t payload = 0;

    // ========== 时间信息 ==========
    uint64_t sendTimestamp = 0;    // 消息发送时间（微秒，来自 Arena）
    uint64_t receiveTimestamp = 0; // 消息被处理的时间（微秒，调度器填充）

    // ========== Payload 解析辅助方法 ==========

    /**
     * @brief 获取延迟（微秒）
     */
    uint64_t GetLatencyUs() const {
        return (receiveTimestamp > sendTimestamp) ? (receiveTimestamp - sendTimestamp) : 0;
    }

    /**
     * @brief 获取延迟（毫秒）
     */
    float GetLatencyMs() const { return GetLatencyUs() / 1000.0f; }

    /**
     * @brief 低位解析（资源句柄 或 单值）
     */
    uint32_t GetLow32() const { return static_cast<uint32_t>(payload & 0xFFFFFFFFULL); }

    /**
     * @brief 高位解析（辅助值）
     */
    uint32_t GetHigh32() const { return static_cast<uint32_t>(payload >> 32); }

    /**
     * @brief 双值解析
     */
    void GetTwoValues(uint32_t &low, uint32_t &high) const {
        low = GetLow32();
        high = GetHigh32();
    }

    /**
     * @brief 单值解析（只使用低32位）
     */
    uint32_t GetSingleValue() const { return GetLow32(); }
};

// ========================================================================
// System 定义
// ========================================================================

/**
 * @brief System函数类型
 *
 * L4层逻辑函数的标准签名
 */
using SystemFunc = std::function<void(ECS::Registry &, const MessageContext &)>;

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
    RenderPhase renderPhase = RenderPhase::Opaque;
    std::vector<SystemId> dependencies;              // 依赖的其他System
    std::vector<MessageTypeHash> interestedMessages; // 感兴趣的消息类型
    bool alwaysRun = false;                          // 常驻标志：每帧都执行，不依赖消息触发
};

} // namespace DX12Engine::Scheduler
