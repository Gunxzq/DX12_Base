#pragma once
#include "Event.h"
#include "EventRegistry.h"

/**
 * @brief MessageArena 使用规范
 *
 * 1. 发送资源句柄 (32-bit Handle):
 *    arena.WriteMessage(i, hash, sender, resourceHandle);
 *    // 读取: uint32_t handle = static_cast<uint32_t>(payloadBuffer[i]);
 *
 * 2. 发送双值事件 (如 WindowResize: Width, Height):
 *    arena.WriteMessage(i, hash, sender, width, height);
 *    // 读取:
 *    //   uint32_t w = static_cast<uint32_t>(payloadBuffer[i] & 0xFFFFFFFF);
 *    //   uint32_t h = static_cast<uint32_t>(payloadBuffer[i] >> 32);
 *
 * 3. 发送复杂结构体:
 *    不建议直接发送大结构体。请将大结构体放入临时对象池，获取其 Handle，
 *    然后按情况 1 发送 Handle。
 */

namespace DX12Engine {
namespace System {
namespace Event {

struct WindowResizeEvent {
    EVENT_HEADER_FIELDS

    // 使用自动生成的枚举值作为 Hash
    static constexpr EventTypeHash StaticTypeHash = static_cast<EventTypeHash>(EventType::WindowResizeEvent);

    uint32_t Width;
    uint32_t Height;
    uint32_t Padding = 0;

    /**
     * @brief 构造函数
     * @param w 新宽度
     * @param h 新高度
     */
    WindowResizeEvent(uint32_t w, uint32_t h)
        : INIT_EVENT_HEADER(EventPriority::P1_High), Width(w), Height(h), Padding(0) {}

    // 禁用默认构造，强制提供宽高
    WindowResizeEvent() = delete;

    // 提供实例方法获取类型哈希 (兼容旧 API)
    inline EventTypeHash GetTypeHash() const { return StaticTypeHash; }
};

struct KeyboardInputEvent {
    EVENT_HEADER_FIELDS

    // 使用自动生成的枚举值作为 Hash
    static constexpr EventTypeHash StaticTypeHash = static_cast<EventTypeHash>(EventType::KeyboardInputEvent);

    uint32_t VirtualKey; // 存储 VK_UP, VK_DOWN 等虚拟键码
    uint32_t Action;     // 0: Pressed, 1: Released (可选，目前主要关注 Pressed)
    uint32_t Padding = 0;

    /**
     * @brief 构造函数
     * @param vk 虚拟键码
     * @param action 动作 (0 for Pressed)
     */
    KeyboardInputEvent(uint32_t vk, uint32_t action = 0)
        : INIT_EVENT_HEADER(EventPriority::P2_Normal), VirtualKey(vk), Action(action), Padding(0) {}

    // 禁用默认构造
    KeyboardInputEvent() = delete;

    // 提供实例方法获取类型哈希
    inline EventTypeHash GetTypeHash() const { return StaticTypeHash; }
};

} // namespace Event
} // namespace System
} // namespace DX12Engine