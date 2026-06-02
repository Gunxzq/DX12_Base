#pragma once
#include "Event.h"

#include "Common/Common.h"

namespace DX12Engine {

namespace Event {

// ========================================================================
// 1. 事件清单定义 (唯一需要手动维护的地方)
//    格式: EVENT_REGISTER(ClassName, HashValue, Priority)
// ========================================================================

#define EVENT_LIST(X)                                                                                                  \
    X(WindowResizeEvent, 0x00000001, P1_High)                                                                          \
    X(KeyboardInputEvent, 0x00000002, P2_Normal)                                                                       \
    X(NetworkPacketEvent, 0x00000100, P2_Normal)                                                                       \
    X(FullscreenToggleEvent, 0x00000101, P1_High)                                                                      \
    /* 异步加载事件（三步数据流） */                                                                                   \
    /* Step 2: 后台加载完成 → ResourceLoaded */                                                                        \
    X(GeometryLoadedEvent, 0x00000200, P4_Background)                                                                  \
    X(MaterialLoadedEvent, 0x00000201, P4_Background)                                                                  \
    X(TextureLoadedEvent, 0x00000202, P4_Background)                                                                   \
    X(TerrainLoadedEvent, 0x00000210, P4_Background)                                                                   \
    /* Step 3: GPU 上传完成 → ResourceUploaded（携带围栏值） */                                                        \
    X(GeometryUploadedEvent, 0x00000300, P4_Background)                                                                \
    X(TextureUploadedEvent, 0x00000301, P4_Background)                                                                 \
    X(TerrainGeometryUploadedEvent, 0x00000310, P4_Background)                                                         \
    /* Step 4: 围栏通过 → ResourceReady */                                                                             \
    X(GeometryReadyEvent, 0x00000400, P3_Low)                                                                          \
    X(TextureReadyEvent, 0x00000401, P3_Low)                                                                           \
    X(TerrainReadyEvent, 0x00000410, P3_Low)                                                                           \
    /* 失败事件 */                                                                                                     \
    X(TerrainLoadFailedEvent, 0x00000211, P4_Background)                                                               \
    X(GeometryLoadFailedEvent, 0x00000220, P4_Background)                                                              \
    X(MaterialLoadFailedEvent, 0x00000221, P4_Background)                                                              \
    X(TextureLoadFailedEvent, 0x00000222, P4_Background)                                                               \
    /* 加载请求与组合完成 */                                                                                   \
    X(RequestLoadEvent, 0x00000230, P2_Normal)                                                                         \
    X(CombineCompleteEvent, 0x00000240, P2_Normal)

// ========================================================================
// 2. 自动生成枚举 (EventType)
// ========================================================================

enum class EventType : uint32_t {
#define GEN_ENUM_ITEM(Name, Hash, Prio) Name = Hash,
    EVENT_LIST(GEN_ENUM_ITEM)
#undef GEN_ENUM_ITEM
        Count // 用于统计事件总数
};

// ========================================================================
// 3. 自动生成名称映射函数 (零开销 Switch-Case)
// ========================================================================

inline const char *GetEventName(EventType type) {
    switch (type) {
#define GEN_CASE_ITEM(Name, Hash, Prio)                                                                                \
    case EventType::Name:                                                                                              \
        return #Name;
        EVENT_LIST(GEN_CASE_ITEM)
#undef GEN_CASE_ITEM
    default:
        return "UnknownEvent";
    }
}

// ========================================================================
// 4. 辅助宏：用于在结构体中自动注入 Hash 和优先级
// ========================================================================

// 获取对应事件的优先级
constexpr EventPriority GetEventPriority(EventType type) {
    switch (type) {
#define GEN_PRIO_ITEM(Name, Hash, Prio)                                                                                \
    case EventType::Name:                                                                                              \
        return EventPriority::Prio;
        EVENT_LIST(GEN_PRIO_ITEM)
#undef GEN_PRIO_ITEM
    default:
        return EventPriority::P4_Background;
    }
}

} // namespace Event

} // namespace DX12Engine