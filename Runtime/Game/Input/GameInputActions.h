#pragma once
// 引入引擎提供的哈希宏
#include "Platform/Input/Core/InputActionId.h"

using DX12Engine::Input::ActionId;

// ------------------------------------------------------------------
// 动作 ID 定义 (Compile-time Hashed IDs)
// 这些变量在编译后就是固定的 uint64_t 数字，例如:
// ActionId_Jump 可能是 0x8A3F2C1E...
// ------------------------------------------------------------------

DEFINE_ACTION(Move);
DEFINE_ACTION(Look);
DEFINE_ACTION(MoveUp);
DEFINE_ACTION(MoveDown);
DEFINE_ACTION(Jump);
DEFINE_ACTION(Sprint);
DEFINE_ACTION(Crouch);
DEFINE_ACTION(Interact);

DEFINE_ACTION(Pause);
DEFINE_ACTION(Confirm);
DEFINE_ACTION(Cancel);

DEFINE_ACTION(ResetCamera);

DEFINE_ACTION(Pick);   // 左键拾取/拖拽
DEFINE_ACTION(Release); // 释放拾取（与 Pick 共用左键）

// ------------------------------------------------------------------
// 辅助函数：获取动作名称用于调试或 JSON 映射
// 注意：JSON 中仍然使用字符串 "Jump"，加载时会再次哈希以匹配这些 ID
// ------------------------------------------------------------------

inline const char *GetActionName(ActionId id) {
    if (id == ActionId_Move)
        return "Move";
    if (id == ActionId_Look)
        return "Look";
    if (id == ActionId_Jump)
        return "Jump";
    if (id == ActionId_Sprint)
        return "Sprint";
    if (id == ActionId_Crouch)
        return "Crouch";
    if (id == ActionId_Interact)
        return "Interact";
    if (id == ActionId_Pause)
        return "Pause";
    if (id == ActionId_Confirm)
        return "Confirm";
    if (id == ActionId_Cancel)
        return "Cancel";

    if (id == ActionId_ResetCamera)
        return "ResetCamera";
    if (id == ActionId_Pick)
        return "Pick";
    if (id == ActionId_Release)
        return "Release";
    return "Unknown";
}
