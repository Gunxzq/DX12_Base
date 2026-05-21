#include "InputSystem.h"
#include "InputConfigLoader.cpp"
#include "RawInputBuffer.h"
#include <algorithm>
#include <cmath>
#include <iostream>

namespace DX12Engine::Input {

constexpr float LONG_PRESS_THRESHOLD = 0.5f; // 长按阈值 (秒)
constexpr float DOUBLE_TAP_INTERVAL = 0.3f;  // 双击最大间隔 (秒)
constexpr float MOUSE_SENSITIVITY = 0.01f;   // 鼠标灵敏度
constexpr float GAMEPAD_DEADZONE = 0.2f;     // 手柄死区

InputSystem &InputSystem::Get() {
    static InputSystem instance;
    return instance;
}

bool InputSystem::Initialize(const std::string &configPath) {
    bool success = InputConfigLoader::LoadConfig(configPath, m_globalBindings, m_contextConfigs);
    if (success) {
        m_contextStack.RegisterContexts(m_contextConfigs);
        m_contextStack.PushContext("Gameplay");

        for (const auto &[id, binding] : m_globalBindings) {
            m_actionStates[id].SetDigital(false, false, false);
            m_prevFrameDigitalState[id] = false;
        }
    }
    return success;
}

void InputSystem::Update(RawInputBuffer &rawBuffer, float deltaTime, float currentTime) {

    // 1. 重置所有动作状态
    for (auto &[id, state] : m_actionStates) {
        // 保存活跃状态用于边缘检测
        bool wasActive = false;
        if (state.Type == EActionValueType::Digital)
            wasActive = state.GetHeld();
        else if (state.Type == EActionValueType::Axis2D) {
            float x, y;
            state.GetAxis2D(x, y);
            wasActive = (std::abs(x) > 0.001f || std::abs(y) > 0.001f);
        }
        m_prevFrameDigitalState[id] = wasActive;

        // 更新长按计时器
        if (state.GetHeld()) {
            state.HoldDuration += deltaTime;
        } else {
            state.HoldDuration = 0.0f;
        }
    }

    // 2. 重置当前帧所有状态
    for (auto &[id, state] : m_actionStates) {
        // 保留时间相关字段 (PressStartTime, LastReleaseTime)，重置瞬时标志
        bool prevLongPress = state.GetLongPressed();
        float prevStartTime = state.PressStartTime;
        float prevLastRelease = state.LastReleaseTime;

        state.SetDigital(false, false, false, false, false, false);
        state.SetAnalog1D(0.0f);
        state.SetAxis2D(0.0f, 0.0f);

        // 恢复时间字段
        state.PressStartTime = prevStartTime;
        state.LastReleaseTime = prevLastRelease;
        // LongPressed 是瞬时事件，每帧重置，由下面逻辑重新触发
    }

    // 3. 评估新输入，填充本帧的 Held/Axis 值
    EvaluateActions(rawBuffer, currentTime);

    for (auto &[id, state] : m_actionStates) {
        if (state.Type != EActionValueType::Digital)
            continue;

        bool currHeld = state.GetHeld();
        bool prevActive = m_prevFrameDigitalState[id];

        // --- 基础边缘检测 ---
        if (currHeld && !prevActive) {
            // Just Pressed
            state.SetDigital(true, false, true, false, false, false);
            state.PressStartTime = currentTime; // 记录按下时间
        } else if (!currHeld && prevActive) {
            // Just Released
            float duration = currentTime - state.PressStartTime;
            bool isTap = (duration < LONG_PRESS_THRESHOLD); // 非长按即为短按

            // 双击检测
            bool isDoubleTap = false;
            if (isTap && (currentTime - state.LastReleaseTime) < DOUBLE_TAP_INTERVAL) {
                isDoubleTap = true;
            }

            state.SetDigital(false, true, false, isTap, isDoubleTap, false);
            state.LastReleaseTime = currentTime; // 更新释放时间
        } else if (currHeld && prevActive) {
            // Held
            bool isLongPress = (state.HoldDuration >= LONG_PRESS_THRESHOLD);

            state.SetDigital(false, false, true, false, false, isLongPress);
        } else {
            // Idle
            state.SetDigital(false, false, false, false, false, false);
        }
    }
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

// --- 核心评估逻辑 ---
void InputSystem::EvaluateActions(RawInputBuffer &rawBuffer, float currentTime) {
    auto activeContexts = m_contextStack.GetActiveContexts();

    for (auto &[actionId, globalBinding] : m_globalBindings) {
        // ... [上下文检查逻辑保持不变] ...
        bool isEnabled = false;
        const ActionBinding *effectiveBinding = &globalBinding;
        for (const auto *ctx : activeContexts) {
            if (std::find(ctx->EnabledActions.begin(), ctx->EnabledActions.end(), actionId) !=
                ctx->EnabledActions.end()) {
                isEnabled = true;
                auto overrideIt = ctx->Overrides.find(actionId);
                if (overrideIt != ctx->Overrides.end())
                    effectiveBinding = &overrideIt->second;
                break;
            }
        }
        if (!isEnabled)
            continue;

        InputActionState &state = m_actionStates[actionId];
        bool anyDigitalKeyDown = false;
        float sumX = 0.0f;
        float sumY = 0.0f;
        bool hasAxisInput = false;

        for (const auto &source : effectiveBinding->Sources) {
            if (source.Axis == BindingSource::AxisType::None) {
                // 数字键
                if (rawBuffer.IsKeyDown(source.KeyCode) &&
                    (source.ModifierKey == EKeyCode::None || rawBuffer.IsKeyDown(source.ModifierKey))) {
                    anyDigitalKeyDown = true;
                }
            } else {
                // 轴向键
                float rawIntensity = 0.0f;
                bool isValidInput = false;

                // 1. 键盘轴 (WASD, Arrows)
                // 使用 KeyCodeUtils::IsKeyboard 更加稳健
                if (KeyCodeUtils::IsKeyboard(source.KeyCode)) {
                    if (rawBuffer.IsKeyDown(source.KeyCode)) {
                        rawIntensity = 1.0f; // 键盘只有按下(1.0)和未按下(0.0)
                        isValidInput = true;
                    }
                }
                // 2. 鼠标轴
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
                // 3. 手柄轴
                else if (KeyCodeUtils::IsAxis(source.KeyCode)) {
                    float rawVal = rawBuffer.GetGamepadAxis(source.KeyCode);

                    // 应用死区
                    if (std::abs(rawVal) > GAMEPAD_DEADZONE) {
                        rawIntensity = rawVal;
                        isValidInput = true;
                    }
                }

                if (isValidInput) {
                    // 【关键修复】统一在这里应用 AxisScale

                    float finalValue = rawIntensity * source.AxisScale;

                    // 对于鼠标，通常不需要额外的死区，但可以加一个极小值过滤噪音
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

        // 死区处理 (针对聚合后的轴)
        if (!KeyCodeUtils::IsKeyboard(effectiveBinding->Sources.empty() ? EKeyCode::None
                                                                        : effectiveBinding->Sources[0].KeyCode)) {
            if (std::abs(sumX) < 0.1f)
                sumX = 0.0f;
            if (std::abs(sumY) < 0.1f)
                sumY = 0.0f;
        }

        if (hasAxisInput) {
            state.SetAxis2D(sumX, sumY);
        } else {
            if (anyDigitalKeyDown) {
                state.SetDigital(false, false, true, false, false, false); // Held = true
            } else {
                state.SetDigital(false, false, false, false, false, false);
            }
        }
    }
}

bool InputSystem::IsKeyboardKey(EKeyCode code) { return KeyCodeUtils::IsKeyboard(code); }

} // namespace DX12Engine