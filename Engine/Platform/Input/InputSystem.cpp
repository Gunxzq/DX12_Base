#include "InputSystem.h"
#include "RawInputBuffer.h"
#include <algorithm>
#include <cmath>

namespace DX12Engine::Input {

constexpr float LONG_PRESS_THRESHOLD = 0.5f; // 长按阈值 (秒)
constexpr float DOUBLE_TAP_INTERVAL = 0.3f;  // 双击最大间隔 (秒)
constexpr float MOUSE_SENSITIVITY = 0.01f;   // 鼠标灵敏度
constexpr float GAMEPAD_DEADZONE = 0.2f;     // 手柄死区
constexpr float AXIS_DEADZONE = 0.1f;        // 轴死区

void InputSystem::SetGlobalBindings(std::unordered_map<ActionId, ActionBinding> &&bindings) {
    m_globalBindings = std::move(bindings);

    for (const auto &[id, binding] : m_globalBindings) {
        m_actionStates[id].SetDigital(false, false, false);
        m_prevFrameActive[id] = false;
    }
}

void InputSystem::Shutdown() {
    m_globalBindings.clear();
    m_actionStates.clear();
    m_prevFrameActive.clear();
}

void InputSystem::Update(const RawInputBuffer &rawBuffer, const std::unordered_set<ActionId> &enabledActions,
                         float deltaTime, float currentTime) {

    // 保存上一帧状态
    SavePreviousFrameState();

    // 更新时间相关数据
    for (auto &[id, state] : m_actionStates) {
        if (state.GetHeld()) {
            state.HoldDuration += deltaTime;
        } else {
            state.HoldDuration = 0.0f;
        }
    }

    // 重置当前帧瞬时状态
    ResetCurrentFrameState();

    // 评估新输入
    EvaluateActions(rawBuffer, enabledActions, currentTime);

    // 边缘检测
    PerformEdgeDetection(currentTime);
}

const InputActionState &InputSystem::GetActionState(ActionId actionId) const {
    auto it = m_actionStates.find(actionId);
    static InputActionState emptyState;
    return (it != m_actionStates.end()) ? it->second : emptyState;
}

FVector2D InputSystem::GetActionAxis2D(ActionId actionId) const {
    const auto &state = GetActionState(actionId);
    float x = 0.0f, y = 0.0f;
    state.GetAxis2D(x, y);
    return {x, y};
}

// ========================================================================
// 内部方法
// ========================================================================

void InputSystem::SavePreviousFrameState() {
    for (auto &[id, state] : m_actionStates) {
        bool wasActive = false;
        if (state.Type == EActionValueType::Digital) {
            wasActive = state.GetHeld();
        } else if (state.Type == EActionValueType::Axis2D) {
            float x, y;
            state.GetAxis2D(x, y);
            wasActive = (std::abs(x) > 0.001f || std::abs(y) > 0.001f);
        }
        m_prevFrameActive[id] = wasActive;
    }
}

void InputSystem::ResetCurrentFrameState() {
    for (auto &[id, state] : m_actionStates) {
        // 保留时间相关字段
        float prevStartTime = state.PressStartTime;
        float prevLastRelease = state.LastReleaseTime;

        state.SetDigital(false, false, false, false, false, false);
        state.SetAnalog1D(0.0f);
        state.SetAxis2D(0.0f, 0.0f);

        state.PressStartTime = prevStartTime;
        state.LastReleaseTime = prevLastRelease;
    }
}

void InputSystem::EvaluateActions(const RawInputBuffer &rawBuffer, const std::unordered_set<ActionId> &enabledActions,
                                  float currentTime) {
    for (ActionId actionId : enabledActions) {
        auto bindingIt = m_globalBindings.find(actionId);
        if (bindingIt == m_globalBindings.end())
            continue;

        const ActionBinding &binding = bindingIt->second;

        InputActionState &state = m_actionStates[actionId];
        bool anyDigitalKeyDown = false;
        float sumX = 0.0f, sumY = 0.0f;
        bool hasAxisInput = false;

        for (const auto &source : binding.Sources) {
            if (source.Axis == BindingSource::AxisType::None) {
                // 数字键处理
                if (rawBuffer.IsKeyDown(source.KeyCode) &&
                    (source.ModifierKey == EKeyCode::None || rawBuffer.IsKeyDown(source.ModifierKey))) {
                    anyDigitalKeyDown = true;
                }
            } else {
                // 轴输入处理
                float rawIntensity = 0.0f;
                bool isValidInput = false;

                // 键盘轴
                if (KeyCodeUtils::IsKeyboard(source.KeyCode)) {
                    if (rawBuffer.IsKeyDown(source.KeyCode)) {
                        rawIntensity = 1.0f;
                        isValidInput = true;
                    }
                }
                // 鼠标轴
                else if (source.KeyCode == EKeyCode::Axis_Mouse_X) {
                    rawIntensity = static_cast<float>(rawBuffer.GetMouseDeltaX());
                    isValidInput = true;
                } else if (source.KeyCode == EKeyCode::Axis_Mouse_Y) {
                    rawIntensity = static_cast<float>(rawBuffer.GetMouseDeltaY());
                    isValidInput = true;
                } else if (source.KeyCode == EKeyCode::Axis_Wheel) {
                    rawIntensity = static_cast<float>(rawBuffer.GetMouseWheelDelta());
                    isValidInput = true;
                }
                // 手柄轴
                else if (KeyCodeUtils::IsAxis(source.KeyCode)) {
                    float rawVal = rawBuffer.GetGamepadAxis(source.KeyCode);

                    // 手柄轴死区处理
                    if (std::abs(rawVal) > GAMEPAD_DEADZONE) {
                        rawIntensity = rawVal;
                        isValidInput = true;
                    }
                }

                if (isValidInput) {
                    float finalValue = rawIntensity * source.AxisScale;

                    if (KeyCodeUtils::IsKeyboard(source.KeyCode) || std::abs(finalValue) > 0.001f) {
                        hasAxisInput = true;
                        if (source.Axis == BindingSource::AxisType::X) {
                            sumX += finalValue;
                        } else if (source.Axis == BindingSource::AxisType::Y) {
                            sumY += finalValue;
                        }
                    }
                }
            }
        }

        // 轴死区处理
        if (std::abs(sumX) < AXIS_DEADZONE)
            sumX = 0.0f;
        if (std::abs(sumY) < AXIS_DEADZONE)
            sumY = 0.0f;

        // 更新状态
        if (hasAxisInput) {
            state.SetAxis2D(sumX, sumY);
        } else {
            if (anyDigitalKeyDown) {
                state.SetDigital(false, false, true, false, false, false);
            } else {
                state.SetDigital(false, false, false, false, false, false);
            }
        }
    }
}

void InputSystem::PerformEdgeDetection(float currentTime) {
    for (auto &[id, state] : m_actionStates) {
        if (state.Type != EActionValueType::Digital)
            continue;

        bool currHeld = state.GetHeld();
        bool prevActive = m_prevFrameActive[id];

        if (currHeld && !prevActive) {
            // 刚按下
            state.SetDigital(true, false, true, false, false, false);
            state.PressStartTime = currentTime;
        } else if (!currHeld && prevActive) {
            // 刚抬起
            float duration = currentTime - state.PressStartTime;
            bool isTap = (duration < LONG_PRESS_THRESHOLD);

            bool isDoubleTap = false;
            if (isTap && (currentTime - state.LastReleaseTime) < DOUBLE_TAP_INTERVAL) {
                isDoubleTap = true;
            }

            state.SetDigital(false, true, false, isTap, isDoubleTap, false);
            state.LastReleaseTime = currentTime;
        } else if (currHeld && prevActive) {
            // 持续按住
            bool isLongPress = (state.HoldDuration >= LONG_PRESS_THRESHOLD);
            state.SetDigital(false, false, true, false, false, isLongPress);
        }
        // 空闲状态：无需操作
    }
}

void InputSystem::ResetAllStates() {
    for (auto &[id, state] : m_actionStates) {
        state.ResetAll();
    }
    for (auto &[id, prev] : m_prevFrameActive) {
        prev = false;
    }
}

} // namespace DX12Engine::Input