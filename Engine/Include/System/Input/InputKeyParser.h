#pragma once
#include "./Core/InputKeyCodes.h"
#include <algorithm>
#include <cctype>
#include <string>
#include <unordered_map>

namespace DX12Engine {
namespace Input {

/**
 * @brief 将 JSON 中的字符串键名解析为 EKeyCode
 * 支持:
 * 1. 字母 (A-Z, a-z) -> Key_A ... Key_Z
 * 2. 数字 (0-9)       -> Key_0 ... Key_9
 * 3. 方向键           -> Key_Up, Key_Down, Key_Left, Key_Right
 * 4. 功能键           -> F1-F12, Escape, Enter, Space, Tab, etc.
 * 5. 修饰键           -> LeftShift, RightCtrl, etc.
 * 6. 手柄             -> Gamepad_A, Gamepad_B, LeftStick, etc.
 */
inline EKeyCode ParseKeyCode(const std::string &keyName) {
    if (keyName.empty())
        return EKeyCode::None;

    // --- 1. 手柄映射 (Handheld/Gamepad) ---
    // 使用静态 Map 处理非标准命名的手柄按键
    static const std::unordered_map<std::string, EKeyCode> GamepadMap = {
        {"Gamepad_A", EKeyCode::Gamepad_A},
        {"Gamepad_B", EKeyCode::Gamepad_B},
        {"Gamepad_X", EKeyCode::Gamepad_X},
        {"Gamepad_Y", EKeyCode::Gamepad_Y},
        {"Gamepad_Start", EKeyCode::Gamepad_Start},
        {"Gamepad_Back", EKeyCode::Gamepad_Back},
        {"Gamepad_LB", EKeyCode::Gamepad_LeftShoulder},
        {"Gamepad_RB", EKeyCode::Gamepad_RightShoulder},
        {"Gamepad_LT", EKeyCode::Gamepad_LeftTrigger},
        {"Gamepad_RT", EKeyCode::Gamepad_RightTrigger},
        {"Gamepad_L3", EKeyCode::Gamepad_LeftThumb},
        {"Gamepad_R3", EKeyCode::Gamepad_RightThumb},
        {"LeftStick", EKeyCode::Axis_LeftStick_X}, // 简化：默认映射到 X 轴，具体轴由上下文决定
        {"RightStick", EKeyCode::Axis_RightStick_X}};

    auto gpIt = GamepadMap.find(keyName);
    if (gpIt != GamepadMap.end()) {
        return gpIt->second;
    }

    // --- 2. 特殊功能键与方向键 (Special Keys) ---
    static const std::unordered_map<std::string, EKeyCode> SpecialKeyMap = {
        // 方向
        {"Up", EKeyCode::Key_Up},
        {"Down", EKeyCode::Key_Down},
        {"Left", EKeyCode::Key_Left},
        {"Right", EKeyCode::Key_Right},

        // 常用功能
        {"Space", EKeyCode::Key_Space},
        {"Enter", EKeyCode::Key_Enter},
        {"Return", EKeyCode::Key_Enter},
        {"Escape", EKeyCode::Key_Escape},
        {"Tab", EKeyCode::Key_Tab},
        {"Backspace", EKeyCode::Key_BackSpace},

        // 修饰键 (区分左右)
        {"LeftShift", EKeyCode::Key_Shift}, // 注意：VK_SHIFT 是通用的，若需区分需用 GetKeyState
        {"RightShift", EKeyCode::Key_Shift},
        {"LeftCtrl", EKeyCode::Key_Ctrl},
        {"RightCtrl", EKeyCode::Key_Ctrl},
        {"LeftAlt", EKeyCode::Key_Alt},
        {"RightAlt", EKeyCode::Key_Alt},

        // 其他
        {"CapsLock", EKeyCode::Key_CapsLock},
        {"Pause", EKeyCode::Key_Pause}};

    auto spIt = SpecialKeyMap.find(keyName);
    if (spIt != SpecialKeyMap.end()) {
        return spIt->second;
    }

    // --- 3. 功能键 F1-F12 ---
    if (keyName.length() == 2 && keyName[0] == 'F') {
        int num = keyName[1] - '0';
        if (num >= 1 && num <= 9) {
            return static_cast<EKeyCode>(VK_F1 + (num - 1));
        }
    }
    if (keyName == "F10")
        return EKeyCode::Key_F10;
    if (keyName == "F11")
        return EKeyCode::Key_F11;
    if (keyName == "F12")
        return EKeyCode::Key_F12;

    // --- 4. 字母键 A-Z (不区分大小写) ---
    if (keyName.length() == 1) {
        char c = std::toupper(keyName[0]);
        if (c >= 'A' && c <= 'Z') {
            return static_cast<EKeyCode>(c); // 对应 InputKeyCodes.h 中的 'A'...'Z'
        }

        // --- 5. 数字键 0-9 ---
        if (c >= '0' && c <= '9') {
            return static_cast<EKeyCode>(c); // 对应 InputKeyCodes.h 中的 '0'...'9'
        }
    }

    // 未识别
    return EKeyCode::None;
}

} // namespace Input
} // namespace DX12Engine