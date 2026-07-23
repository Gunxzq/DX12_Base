#pragma once
#include "Core/InputKeyCodes.h"

#include "Common/Common.h"
#include <array>

namespace DX12Engine {
namespace Input {

/**
 * @brief 原始输入缓冲区
 *
 * 职责：
 * 1. 存储当前帧的原始按键状态（按下/抬起）。
 * 2. 存储鼠标增量移动和滚轮数据。
 * 3. 存储手柄摇杆轴值。
 */
class RawInputBuffer {
public:
    void init(bool isModKeySeparated = false) { m_isModKeySeparated = isModKeySeparated; };

    RawInputBuffer() { Reset(); }

    // ==========================
    // 帧管理
    // ==========================

    /**
     * @brief 每帧开始时调用，重置增量数据（鼠标Delta、滚轮等）
     * 按键的持续状态（Pressed/Released）保持不变
     */
    void BeginFrame() {
        m_mouseDeltaX = 0;
        m_mouseDeltaY = 0;
        m_mouseWheelDelta = 0;
    }

    void Reset() {
        m_keyStates.fill(false);
        m_gamepadAxes.fill(0.0f);
        m_mouseX = 0;
        m_mouseY = 0;
        m_mouseDeltaX = 0;
        m_mouseDeltaY = 0;
        m_mouseWheelDelta = 0;
    }

    // ==========================
    // 事件注入 (由 WindowProc 或平台层调用)
    // ==========================

    void OnKeyDown(EKeyCode rawCode) {
        EKeyCode norm = rawCode;
        if (m_isModKeySeparated) {
            norm = KeyCodeUtils::Normalize(rawCode);
        }
        if (norm != EKeyCode::None && IsValidKeyCode(norm)) {
            m_keyStates[static_cast<size_t>(norm)] = true;
        }
    }

    void OnKeyUp(EKeyCode rawCode) {
        EKeyCode norm = rawCode;
        if (m_isModKeySeparated) {
            norm = KeyCodeUtils::Normalize(rawCode);
        }
        if (norm != EKeyCode::None && IsValidKeyCode(norm)) {
            m_keyStates[static_cast<size_t>(norm)] = false;
        }
    }

    /**
     * @brief 记录鼠标绝对位置，并自动计算 Delta
     * @param x 屏幕/窗口坐标 X
     * @param y 屏幕/窗口坐标 Y
     */
    void OnMouseMove(int x, int y) {
        m_mouseDeltaX = x - m_mouseX;
        m_mouseDeltaY = y - m_mouseY;
        m_mouseX = x;
        m_mouseY = y;
    }

    /**
     * @brief 累积鼠标滚轮滚动量
     * @param delta 滚动增量 (通常 WHEEL_DELTA 为 120)
     */
    void OnMouseWheel(int delta) { m_mouseWheelDelta += delta; }

    /**
     * @brief 设置手柄轴值 (由 XInput 轮询更新)
     * @param axis 轴对应的 EKeyCode (如 Axis_LeftStick_X)
     * @param value 归一化值 [-1.0, 1.0]
     */
    void SetGamepadAxis(EKeyCode axis, float value) {
        if (IsValidKeyCode(axis)) {
            m_gamepadAxes[static_cast<size_t>(axis)] = value;
        }
    }

    // ==========================
    // 状态查询 (供 InputSystem 使用)
    // ==========================

    /**
     * @brief 查询按键当前是否处于按下状态
     */
    bool IsKeyDown(EKeyCode code) const {
        if (!IsValidKeyCode(code))
            return false;
        return m_keyStates[static_cast<size_t>(code)];
    }

    int GetMouseDeltaX() const { return m_mouseDeltaX; }
    int GetMouseDeltaY() const { return m_mouseDeltaY; }
    int GetMouseWheelDelta() const { return m_mouseWheelDelta; }

    float GetGamepadAxis(EKeyCode axis) const {
        if (!IsValidKeyCode(axis))
            return 0.0f;
        return m_gamepadAxes[static_cast<size_t>(axis)];
    }

    void ResetMouseDelta() {
        m_mouseDeltaX = 0;
        m_mouseDeltaY = 0;
    }

private:
    // 辅助函数：防止数组越界
    static constexpr size_t KEY_COUNT = 4096;

    inline bool IsValidKeyCode(EKeyCode code) const { return static_cast<size_t>(code) < KEY_COUNT; }

    // 键盘状态表
    std::array<bool, KEY_COUNT> m_keyStates{};

    // 手柄轴值表 (复用 KEY_COUNT 大小以简化索引，虽然浪费空间但避免额外映射)
    std::array<float, KEY_COUNT> m_gamepadAxes{};

    // 鼠标状态
    int m_mouseX = 0, m_mouseY = 0;
    int m_mouseDeltaX = 0, m_mouseDeltaY = 0;
    int m_mouseWheelDelta = 0;

    // 是否区分修饰键（Shift, Ctrl, Alt）
    bool m_isModKeySeparated = false;
};

} // namespace Input
} // namespace DX12Engine