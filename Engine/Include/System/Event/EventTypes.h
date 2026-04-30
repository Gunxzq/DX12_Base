#pragma once
#include "System/Event/Event.h"
#include "System/Event/EventRegistry.h" // 引入注册表

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

} // namespace Event
} // namespace System
} // namespace DX12Engine