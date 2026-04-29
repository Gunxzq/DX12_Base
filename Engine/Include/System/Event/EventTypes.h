#pragma once

#include "System/Event/Event.h"
#include <cstdint>

namespace DX12Engine {
namespace System {
namespace Event {

/**
 * @brief 窗口大小改变事件
 *
 * 当用户调整窗口大小或切换全屏模式时触发。
 * 优先级设为 P1_High，因为需要同步层检查 GPU 状态并重建交换链。
 */
struct WindowResizeEvent {
    // 1. 插入标准头部字段
    EVENT_HEADER_FIELDS

    // 2. 定义类型哈希
    DEFINE_EVENT_TYPE_HASH(0x00000001)

    // 3. 事件特有数据
    uint32_t Width;
    uint32_t Height;

    // 4. 显式填充字段（确保 sizeof = 12，无隐藏填充）
    uint32_t Padding = 0;

    /**
     * @brief 构造函数
     * @param w 新宽度
     * @param h 新高度
     */
    WindowResizeEvent(uint32_t w, uint32_t h) : INIT_EVENT_HEADER(EventPriority::P1_High), Width(w), Height(h), Padding(0) {}

    // 禁用默认构造，强制提供宽高
    WindowResizeEvent() = delete;
};

} // namespace Event
} // namespace System
} // namespace DX12Engine