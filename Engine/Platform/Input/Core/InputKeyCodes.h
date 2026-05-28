#pragma once

#include "Common/Common.h"

namespace DX12Engine {
namespace Input {

/**
 * @brief Windows 平台统一的输入键码枚举
 *
 * 设计原则：
 * 1. 键盘部分直接对齐 Windows Virtual Key Codes (VK_)，减少转换开销。
 * 2. 鼠标和手柄使用偏移量，避免与 VK_ 冲突。
 * 3. 涵盖 Keyboard, Mouse, Gamepad (XInput)。
 */
enum class EKeyCode : uint16_t {
    None = 0,

    // ------------------------------------------------------------------
    // 1. 键盘 (Keyboard) - 直接对应 VK_ 值
    // 范围: 0x08 - 0xFE (常用值在 0x41-0x5A 为 A-Z)
    // ------------------------------------------------------------------

    // 常用功能键
    Key_BackSpace = VK_BACK,   // 8
    Key_Tab = VK_TAB,          // 9
    Key_Enter = VK_RETURN,     // 13
    Key_Shift = VK_SHIFT,      // 16 (通用 Shift)
    Key_Ctrl = VK_CONTROL,     // 17 (通用 Ctrl)
    Key_Alt = VK_MENU,         // 18 (通用 Alt)
    Key_Pause = VK_PAUSE,      // 19
    Key_CapsLock = VK_CAPITAL, // 20
    Key_Escape = VK_ESCAPE,    // 27
    Key_Space = VK_SPACE,      // 32

    // 方向键与编辑键
    Key_Left = VK_LEFT,   // 37
    Key_Up = VK_UP,       // 38
    Key_Right = VK_RIGHT, // 39
    Key_Down = VK_DOWN,   // 40

    // 字母键 A-Z
    // 修复说明：Windows 没有 VK_A 等宏，虚拟键码直接等于 ASCII 字符值
    Key_A = 'A',
    Key_B = 'B',
    Key_C = 'C',
    Key_D = 'D',
    Key_E = 'E',
    Key_F = 'F',
    Key_G = 'G',
    Key_H = 'H',
    Key_I = 'I',
    Key_J = 'J',
    Key_K = 'K',
    Key_L = 'L',
    Key_M = 'M',
    Key_N = 'N',
    Key_O = 'O',
    Key_P = 'P',
    Key_Q = 'Q',
    Key_R = 'R',
    Key_S = 'S',
    Key_T = 'T',
    Key_U = 'U',
    Key_V = 'V',
    Key_W = 'W',
    Key_X = 'X',
    Key_Y = 'Y',
    Key_Z = 'Z',

    // 数字键 0-9
    // 修复说明：同上，使用字符字面量
    Key_0 = '0',
    Key_1 = '1',
    Key_2 = '2',
    Key_3 = '3',
    Key_4 = '4',
    Key_5 = '5',
    Key_6 = '6',
    Key_7 = '7',
    Key_8 = '8',
    Key_9 = '9',

    // F1-F12
    Key_F1 = VK_F1,
    Key_F2 = VK_F2,
    Key_F3 = VK_F3,
    Key_F4 = VK_F4,
    Key_F5 = VK_F5,
    Key_F6 = VK_F6,
    Key_F7 = VK_F7,
    Key_F8 = VK_F8,
    Key_F9 = VK_F9,
    Key_F10 = VK_F10,
    Key_F11 = VK_F11,
    Key_F12 = VK_F12,

    // ------------------------------------------------------------------
    // 2. 鼠标 (Mouse) - 使用偏移量 1000+
    // ------------------------------------------------------------------
    Mouse_Left = 1000,
    Mouse_Right = 1001,
    Mouse_Middle = 1002,
    Mouse_X1 = 1003, // 侧键1
    Mouse_X2 = 1004, // 侧键2

    // 鼠标滚轮通常作为 Axis 处理，但如果需要作为按钮触发也可以加
    Mouse_WheelUp = 1005,
    Mouse_WheelDown = 1006,

    // ------------------------------------------------------------------
    // 3. 手柄 (Gamepad - XInput) - 使用偏移量 2000+
    // 参考 XINPUT_GAMEPAD_ 定义
    // ------------------------------------------------------------------
    Gamepad_DPad_Up = 2000,
    Gamepad_DPad_Down = 2001,
    Gamepad_DPad_Left = 2002,
    Gamepad_DPad_Right = 2003,

    Gamepad_Start = 2004,
    Gamepad_Back = 2005,

    Gamepad_LeftThumb = 2006,  // L3 按下
    Gamepad_RightThumb = 2007, // R3 按下

    Gamepad_LeftShoulder = 2008,  // LB
    Gamepad_RightShoulder = 2009, // RB

    Gamepad_A = 2010,
    Gamepad_B = 2011,
    Gamepad_X = 2012,
    Gamepad_Y = 2013,

    // 扳机键 (通常作为 Axis，但也可作为 Digital 阈值触发)
    Gamepad_LeftTrigger = 2014,
    Gamepad_RightTrigger = 2015,

    // ------------------------------------------------------------------
    // 4. 模拟轴 (Axes) - 使用偏移量 3000+
    // 这些不是“按键”，而是“通道”，用于 Analog/Axis2D 类型的 Action
    // ------------------------------------------------------------------
    Axis_LeftStick_X = 3000,
    Axis_LeftStick_Y = 3001,
    Axis_RightStick_X = 3002,
    Axis_RightStick_Y = 3003,

    // 如果希望扳机作为纯模拟量（0.0-1.0）而非按键
    Axis_LeftTrigger = 3004,
    Axis_RightTrigger = 3005,

    Axis_Mouse_X = 3006, // 鼠标移动 Delta X
    Axis_Mouse_Y = 3007, // 鼠标移动 Delta Y
    Axis_Wheel = 3008,   // 滚轮滚动量

    Max
};

/**
 * @brief 简单的工具函数，判断键码所属的设备类型
 */
namespace KeyCodeUtils {
static inline bool IsKeyboard(EKeyCode code) {
    uint16_t v = static_cast<uint16_t>(code);
    return v > 0 && v < 1000;
}

static inline bool IsMouse(EKeyCode code) {
    uint16_t v = static_cast<uint16_t>(code);
    return v >= 1000 && v < 2000;
}

static inline bool IsGamepadButton(EKeyCode code) {
    uint16_t v = static_cast<uint16_t>(code);
    return v >= 2000 && v < 3000;
}

static inline bool IsAxis(EKeyCode code) {
    uint16_t v = static_cast<uint16_t>(code);
    return v >= 3000 && v < 4000;
}

/**
 * @brief 获取用于调试显示的字符串名称
 */
static inline const char *ToString(EKeyCode code) {
    // 实际项目中建议实现一个完整的映射表，这里仅做示例
    if (IsKeyboard(code))
        return "Keyboard Key";
    if (IsMouse(code))
        return "Mouse Button";
    if (IsGamepadButton(code))
        return "Gamepad Button";
    if (IsAxis(code))
        return "Axis";
    return "Unknown";
}
} // namespace KeyCodeUtils

} // namespace Input
} // namespace DX12Engine