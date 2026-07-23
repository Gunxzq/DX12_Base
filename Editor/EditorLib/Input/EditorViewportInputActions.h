#pragma once

#include "Platform/Input/Core/InputActionId.h"

// ========================================================================
// 编辑器视口输入动作 ID 定义
// 与 Editor/Config/default_input.json 中的 bindings 名称对应
// ========================================================================

namespace DX12Engine::Input {

DEFINE_ACTION(Move);
DEFINE_ACTION(Look);
DEFINE_ACTION(MoveUp);
DEFINE_ACTION(MoveDown);
DEFINE_ACTION(Sprint);
DEFINE_ACTION(Select);
DEFINE_ACTION(OrbitCamera);
DEFINE_ACTION(Zoom);
DEFINE_ACTION(Pan);
DEFINE_ACTION(FocusSelection);
DEFINE_ACTION(ToolCursor);
DEFINE_ACTION(ToolTranslate);
DEFINE_ACTION(ToolRotate);

} // namespace DX12Engine::Input