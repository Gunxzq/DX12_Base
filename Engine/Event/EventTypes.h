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

struct FullscreenToggleEvent {
    EVENT_HEADER_FIELDS

    // 使用自动生成的枚举值作为 Hash
    static constexpr EventTypeHash StaticTypeHash = static_cast<EventTypeHash>(EventType::FullscreenToggleEvent);

    bool bFullscreen; // true = 切换至全屏, false = 切换至窗口
    uint32_t Padding; // 对齐填充

    /**
     * @brief 构造函数
     * @param bFull 是否切换到全屏模式
     */
    FullscreenToggleEvent(bool bFull) : INIT_EVENT_HEADER(EventPriority::P1_High), bFullscreen(bFull), Padding(0) {}

    // 禁用默认构造
    FullscreenToggleEvent() = delete;

    // 提供实例方法获取类型哈希
    inline EventTypeHash GetTypeHash() const { return StaticTypeHash; }
};

// ========================================================================
// 异步加载事件（三步数据流，资源类型由 payload 高位编码）
// 均无额外字段，数据通过 payload 传递
// ========================================================================

struct ResourceLoadCompleteEvent {
    EVENT_HEADER_FIELDS
    static constexpr EventTypeHash StaticTypeHash = static_cast<EventTypeHash>(EventType::ResourceLoadCompleteEvent);

    explicit ResourceLoadCompleteEvent(EventPriority prio = EventPriority::P4_Background)
        : INIT_EVENT_HEADER(prio) {}

    inline EventTypeHash GetTypeHash() const { return StaticTypeHash; }
};

struct ResourceUploadCompleteEvent {
    EVENT_HEADER_FIELDS
    static constexpr EventTypeHash StaticTypeHash = static_cast<EventTypeHash>(EventType::ResourceUploadCompleteEvent);

    explicit ResourceUploadCompleteEvent(EventPriority prio = EventPriority::P4_Background)
        : INIT_EVENT_HEADER(prio) {}

    inline EventTypeHash GetTypeHash() const { return StaticTypeHash; }
};

struct ResourceReadyEvent {
    EVENT_HEADER_FIELDS
    static constexpr EventTypeHash StaticTypeHash = static_cast<EventTypeHash>(EventType::ResourceReadyEvent);

    explicit ResourceReadyEvent(EventPriority prio = EventPriority::P3_Low)
        : INIT_EVENT_HEADER(prio) {}

    inline EventTypeHash GetTypeHash() const { return StaticTypeHash; }
};

struct ResourceLoadFailedEvent {
    EVENT_HEADER_FIELDS
    static constexpr EventTypeHash StaticTypeHash = static_cast<EventTypeHash>(EventType::ResourceLoadFailedEvent);

    explicit ResourceLoadFailedEvent(EventPriority prio = EventPriority::P4_Background)
        : INIT_EVENT_HEADER(prio) {}

    inline EventTypeHash GetTypeHash() const { return StaticTypeHash; }
};

struct RequestLoadEvent {
    EVENT_HEADER_FIELDS
    static constexpr EventTypeHash StaticTypeHash = static_cast<EventTypeHash>(EventType::RequestLoadEvent);

    explicit RequestLoadEvent(EventPriority prio = EventPriority::P2_Normal)
        : INIT_EVENT_HEADER(prio) {}

    inline EventTypeHash GetTypeHash() const { return StaticTypeHash; }
};

struct CombineCompleteEvent {
    EVENT_HEADER_FIELDS
    static constexpr EventTypeHash StaticTypeHash = static_cast<EventTypeHash>(EventType::CombineCompleteEvent);

    explicit CombineCompleteEvent(EventPriority prio = EventPriority::P2_Normal)
        : INIT_EVENT_HEADER(prio) {}

    inline EventTypeHash GetTypeHash() const { return StaticTypeHash; }
};

// ========================================================================
// Asset Loaded Payload Utilities (RequestID + Handle)
// ========================================================================

// 自定义 64 位布局:
// Bits 0-31:  handle.index (32 bits)
// Bits 32-41: handle.generation (10 bits)
// Bits 42-63: requestId (22 bits, 最大支持约 400 万并发请求)

constexpr uint64_t ASSET_PAYLOAD_REQUESTID_MASK = 0x3FFFFF; // 22 bits
constexpr uint64_t ASSET_PAYLOAD_GENERATION_MASK = 0x3FF;   // 10 bits
constexpr uint64_t ASSET_PAYLOAD_INDEX_MASK = 0xFFFFFFFF;   // 32 bits

constexpr int ASSET_PAYLOAD_REQUESTID_SHIFT = 42;
constexpr int ASSET_PAYLOAD_GENERATION_SHIFT = 32;
constexpr int ASSET_PAYLOAD_INDEX_SHIFT = 0;

inline uint64_t MakeAssetLoadedPayload(uint32_t requestId, uint32_t handleIndex, uint32_t handleGen) {
    uint64_t payload = 0;
    payload |= (static_cast<uint64_t>(requestId) & ASSET_PAYLOAD_REQUESTID_MASK) << ASSET_PAYLOAD_REQUESTID_SHIFT;
    payload |= (static_cast<uint64_t>(handleGen) & ASSET_PAYLOAD_GENERATION_MASK) << ASSET_PAYLOAD_GENERATION_SHIFT;
    payload |= static_cast<uint64_t>(handleIndex) & ASSET_PAYLOAD_INDEX_MASK;
    return payload;
}

inline void DecodeAssetLoadedPayload(uint64_t payload, uint32_t &outRequestId, uint32_t &outHandleIdx,
                                     uint32_t &outHandleGen) {
    outRequestId = static_cast<uint32_t>((payload >> ASSET_PAYLOAD_REQUESTID_SHIFT) & ASSET_PAYLOAD_REQUESTID_MASK);
    outHandleGen = static_cast<uint32_t>((payload >> ASSET_PAYLOAD_GENERATION_SHIFT) & ASSET_PAYLOAD_GENERATION_MASK);
    outHandleIdx = static_cast<uint32_t>(payload & ASSET_PAYLOAD_INDEX_MASK);
}

template <typename HandleType> inline HandleType DecodeAssetHandle(uint64_t payload) {
    uint32_t idx = static_cast<uint32_t>(payload & ASSET_PAYLOAD_INDEX_MASK);
    uint32_t gen = static_cast<uint32_t>((payload >> ASSET_PAYLOAD_GENERATION_SHIFT) & ASSET_PAYLOAD_GENERATION_MASK);
    return HandleType::FromParts(idx, gen);
}

} // namespace Event

} // namespace DX12Engine