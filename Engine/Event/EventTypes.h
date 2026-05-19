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

// ========================================================================
// Network Payload Utilities (P0 Stage)
// ========================================================================

/**
 * 64 位负载布局:
 *
 * 直接模式 (Bit 63 = 0):
 * ┌────────┬────────────┬──────────────────────┬──────────────────────┐
 * │ Bit 63 │ Bits 56-62 │     Bits 32-55        │     Bits 0-31        │
 * ├────────┼────────────┼──────────────────────┼──────────────────────┤
 * │   0    │  消息类型   │      高24位数据       │      低32位数据      │
 * │        │  (7 bits)  │     (24 bits)         │      (32 bits)       │
 * └────────┴────────────┴──────────────────────┴──────────────────────┘
 *
 * Handle 模式 (Bit 63 = 1):
 * ┌────────┬────────────┬─────────────────────────────────────────────┐
 * │ Bit 63 │ Bits 56-62 │                 Bits 0-55                    │
 * ├────────┼────────────┼─────────────────────────────────────────────┤
 * │   1    │  消息类型   │              ResourceHandle                 │
 * │        │  (7 bits)  │                (56 bits)                    │
 * └────────┴────────────┴─────────────────────────────────────────────┘
 */

//  标志位
constexpr uint64_t NETWORK_PAYLOAD_HANDLE_FLAG = 1ULL << 63;

// 消息类型字段 (Bits 56-62, 7 bits)
constexpr uint64_t NETWORK_PAYLOAD_TYPE_MASK = 0x7FULL; // 0b1111111
constexpr int NETWORK_PAYLOAD_TYPE_SHIFT = 56;

// 直接模式数据字段
// Low Data: Bits 0-31 (32 bits)
constexpr uint64_t NETWORK_PAYLOAD_LOW_DATA_MASK = 0xFFFFFFFFULL;
// High Data: Bits 32-55 (24 bits) -> 注意：这里只使用24位，最高8位(相对于32位偏移)需确保为0或忽略
constexpr uint64_t NETWORK_PAYLOAD_HIGH_DATA_MASK = 0xFFFFFFULL; // 24 bits
constexpr int NETWORK_PAYLOAD_HIGH_DATA_SHIFT = 32;

// Handle 模式数据字段
// Handle: Bits 0-55 (56 bits)
constexpr uint64_t NETWORK_PAYLOAD_HANDLE_MASK = 0x00FFFFFFFFFFFFFFULL; // 56 bits

/**
 * @brief 创建直接存储的小包 Payload
 * @param messageType 消息类型 (7 bits, 0-127)
 * @param highVal     高24位数据 (例如: FrameNum 的部分位, Timestamp 的部分位)
 * @param lowVal      低32位数据 (例如: InputData, Sequence)
 * @return uint64_t   编码后的 Payload
 */
inline uint64_t MakeNetworkDirectPayload(uint8_t messageType, uint32_t highVal, uint32_t lowVal) {
    // 1. 确保 messageType 只有7位
    uint64_t typePart = (static_cast<uint64_t>(messageType) & NETWORK_PAYLOAD_TYPE_MASK) << NETWORK_PAYLOAD_TYPE_SHIFT;

    // 2. 确保 highVal 只有24位 (Bits 32-55)
    uint64_t highPart = (static_cast<uint64_t>(highVal) & NETWORK_PAYLOAD_HIGH_DATA_MASK)
                        << NETWORK_PAYLOAD_HIGH_DATA_SHIFT;

    // 3. 确保 lowVal 只有32位 (Bits 0-31)
    uint64_t lowPart = static_cast<uint64_t>(lowVal) & NETWORK_PAYLOAD_LOW_DATA_MASK;

    // 4. 组合 (Bit 63 默认为 0)
    return typePart | highPart | lowPart;
}

/**
 * @brief 创建 Handle 引用的大包 Payload
 * @param messageType 消息类型 (7 bits, 0-127)
 * @param handle      资源句柄 (最多56 bits)
 * @return uint64_t   编码后的 Payload
 */
inline uint64_t MakeNetworkHandlePayload(uint8_t messageType, uint64_t handle) {
    // 1. 设置最高位为1 (Handle模式)
    uint64_t flag = NETWORK_PAYLOAD_HANDLE_FLAG;

    // 2. 设置消息类型 (Bits 56-62)
    uint64_t typePart = (static_cast<uint64_t>(messageType) & NETWORK_PAYLOAD_TYPE_MASK) << NETWORK_PAYLOAD_TYPE_SHIFT;

    // 3. 设置 Handle (Bits 0-55)
    uint64_t handlePart = handle & NETWORK_PAYLOAD_HANDLE_MASK;

    return flag | typePart | handlePart;
}

/**
 * @brief 判断 Payload 是否为 Handle 引用
 */
inline bool IsNetworkHandlePayload(uint64_t payload) { return (payload & NETWORK_PAYLOAD_HANDLE_FLAG) != 0; }

/**
 * @brief 从 Payload 中提取消息类型 (7 bits)
 */
inline uint8_t DecodeNetworkMessageType(uint64_t payload) {
    return static_cast<uint8_t>((payload >> NETWORK_PAYLOAD_TYPE_SHIFT) & NETWORK_PAYLOAD_TYPE_MASK);
}

/**
 * @brief 从 Handle Payload 中提取句柄值 (56 bits)
 */
inline uint64_t DecodeNetworkHandle(uint64_t payload) {
    // 清除 Bit 63 和 Bits 56-62
    return payload & NETWORK_PAYLOAD_HANDLE_MASK;
}

/**
 * @brief 从直接 Payload 中提取高低数据
 * @return pair<High24Bits, Low32Bits>
 */
inline std::pair<uint32_t, uint32_t> DecodeNetworkDirectPayload(uint64_t payload) {
    // 提取 Low 32 bits (0-31)
    uint32_t lowVal = static_cast<uint32_t>(payload & NETWORK_PAYLOAD_LOW_DATA_MASK);

    // 提取 High 24 bits (32-55)
    uint32_t highVal =
        static_cast<uint32_t>((payload >> NETWORK_PAYLOAD_HIGH_DATA_SHIFT) & NETWORK_PAYLOAD_HIGH_DATA_MASK);

    return {highVal, lowVal};
}

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

struct NetworkPacketEvent {
    EVENT_HEADER_FIELDS

    // 使用自动生成的枚举值作为 Hash
    static constexpr EventTypeHash StaticTypeHash = static_cast<EventTypeHash>(EventType::NetworkPacketEvent);

    /**
     * @brief 构造函数 (仅用于初始化 Header，实际数据通过 Payload 传递)
     * @param prio 优先级
     */
    explicit NetworkPacketEvent(EventPriority prio = EventPriority::P2_Normal) : INIT_EVENT_HEADER(prio) {}

    // 提供实例方法获取类型哈希
    inline EventTypeHash GetTypeHash() const { return StaticTypeHash; }
};

} // namespace Event
} // namespace System
} // namespace DX12Engine