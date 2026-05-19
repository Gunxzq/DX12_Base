#pragma once
#include "InputActionId.h"
#include "InputKeyCodes.h"
#include <string>
#include <unordered_map>
#include <vector>

namespace DX12Engine {
namespace Input {

/**
 * @brief 单个输入源的定义
 */
struct BindingSource {
    // 主输入
    EKeyCode KeyCode = EKeyCode::None;

    // 修饰键（键盘 Shift/Ctrl/Alt，手柄 LB/LT，鼠标侧键等）
    EKeyCode ModifierKey = EKeyCode::None;

    // 轴向类型
    enum class AxisType { None, X, Y, Trigger, Wheel };
    AxisType Axis = AxisType::None;

    float AxisScale = 0.0f; // +1 / -1 / 阈值

    // 模拟阈值（扳机、滚轮等）
    float Threshold = 0.0f;

    // 设备类型
    enum class DeviceType { Auto, Keyboard, Mouse, Gamepad };
    DeviceType Device = DeviceType::Auto;

    BindingSource(EKeyCode key, EKeyCode modifier, AxisType axis, float scale)
        : KeyCode(key), ModifierKey(modifier), Axis(axis), AxisScale(scale) {}

    BindingSource() = default;
    explicit BindingSource(EKeyCode code) : KeyCode(code) {}

    // ========== 键盘 ==========
    static BindingSource Key(EKeyCode key) {
        BindingSource src;
        src.Device = DeviceType::Keyboard;
        src.KeyCode = key;
        return src;
    }

    static BindingSource KeyChord(EKeyCode key, EKeyCode modifier) {
        BindingSource src;
        src.Device = DeviceType::Keyboard;
        src.KeyCode = key;
        src.ModifierKey = modifier;
        return src;
    }

    static BindingSource KeyboardAxis(EKeyCode key, AxisType axis, float scale) {
        BindingSource src;
        src.Device = DeviceType::Keyboard;
        src.KeyCode = key;
        src.Axis = axis;
        src.AxisScale = scale;
        return src;
    }

    // ========== 鼠标 ==========
    static BindingSource MouseButton(EKeyCode btn) {
        BindingSource src;
        src.Device = DeviceType::Mouse;
        src.KeyCode = btn;
        return src;
    }

    static BindingSource MouseButtonChord(EKeyCode btn, EKeyCode modifier) {
        BindingSource src;
        src.Device = DeviceType::Mouse;
        src.KeyCode = btn;
        src.ModifierKey = modifier;
        return src;
    }

    static BindingSource MouseAxis(EKeyCode axis, float scale = 1.0f) {
        BindingSource src;
        src.Device = DeviceType::Mouse;
        src.KeyCode = axis; // Axis_Mouse_X, Axis_Mouse_Y
        src.Axis = (axis == EKeyCode::Axis_Mouse_X) ? AxisType::X : AxisType::Y;
        src.AxisScale = scale;
        return src;
    }

    static BindingSource MouseWheel(float scale = 1.0f) {
        BindingSource src;
        src.Device = DeviceType::Mouse;
        src.KeyCode = EKeyCode::Axis_Wheel;
        src.Axis = AxisType::Wheel;
        src.AxisScale = scale;
        return src;
    }

    // ========== 手柄 ==========
    static BindingSource GamepadButton(EKeyCode btn) {
        BindingSource src;
        src.Device = DeviceType::Gamepad;
        src.KeyCode = btn;
        return src;
    }

    static BindingSource GamepadChord(EKeyCode btn, EKeyCode modifier) {
        BindingSource src;
        src.Device = DeviceType::Gamepad;
        src.KeyCode = btn;
        src.ModifierKey = modifier;
        return src;
    }

    static BindingSource GamepadTrigger(EKeyCode trigger, float threshold = 0.5f) {
        BindingSource src;
        src.Device = DeviceType::Gamepad;
        src.KeyCode = trigger;
        src.Axis = AxisType::Trigger;
        src.Threshold = threshold;
        return src;
    }

    static BindingSource GamepadStick(EKeyCode axis, AxisType axisType, float scale = 1.0f) {
        BindingSource src;
        src.Device = DeviceType::Gamepad;
        src.KeyCode = axis;
        src.Axis = axisType;
        src.AxisScale = scale;
        return src;
    }
};
/**
 * @brief 动作绑定集合
 */
struct ActionBinding {
    ActionId Id;
    std::vector<BindingSource> Sources;
};

/**
 * @brief 上下文配置
 */
struct InputContextConfig {
    std::string Name;
    int Priority = 0;
    std::vector<ActionId> EnabledActions;

    // 局部覆盖：Key 是 ActionId, Value 是该动作在此上下文中的新绑定
    std::unordered_map<ActionId, ActionBinding> Overrides;
};

} // namespace Input
} // namespace DX12Engine