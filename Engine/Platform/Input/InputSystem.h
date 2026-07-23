#pragma once
#include "Core/InputActionId.h"
#include "Core/InputActionState.h"
#include "Core/InputBinding.h"
#include "Core/InputKeyCodes.h"
#include "Math/MathTypes.h"
#include <functional>
#include <unordered_map>
#include <unordered_set>

// 前置声明
namespace DX12Engine {
namespace Input {
class RawInputBuffer;

enum class TriggerBehavior : uint8_t {
    WhileHeld,     // 按住时持续触发
    OnPressed,     // 按下时触发一次
    OnReleased,    // 释放时触发
    Axis2D,        // 二维轴变化时触发
    OnTapped,      // 短按时触发（按下后快速释放）
    OnDoubleTap,   // 双击时触发
    OnHoldRelease, // 长按释放时触发
    OnRepeat,      // 重复触发（按住时按间隔重复）
    Analog1D,      // 一维轴变化时触发
};

/// 输入回调 ID
using ActionCallbackId = uint32_t;

class InputSystem {

public:
    void SetGlobalBindings(std::unordered_map<ActionId, ActionBinding> &&bindings);
    void Shutdown();

    void Update(const RawInputBuffer &rawBuffer, const std::unordered_set<ActionId> &enabledActions, float deltaTime,
                float currentTime);

    const InputActionState &GetActionState(ActionId actionId) const;
    bool IsActionPressed(ActionId actionId) const { return GetActionState(actionId).GetPressed(); };
    bool IsActionReleased(ActionId actionId) const { return GetActionState(actionId).GetReleased(); };
    bool IsActionHeld(ActionId actionId) const { return GetActionState(actionId).GetHeld(); };
    FVector2D GetActionAxis2D(ActionId actionId) const;

    // 注册输入回调：当 Action 满足触发条件时自动调用
    ActionCallbackId BindCallback(ActionId actionId, std::function<void(const InputActionState &)> callback,
                                  TriggerBehavior trigger = TriggerBehavior::OnPressed);

    // 注销输入回调
    void UnbindCallback(ActionCallbackId id);

    void ResetAllStates();

private:
    void EvaluateActions(const RawInputBuffer &rawBuffer, const std::unordered_set<ActionId> &enabledActions,
                         float currentTime);
    void PerformEdgeDetection(float currentTime);
    void SavePreviousFrameState();
    void ResetCurrentFrameState();
    void InvokeCallbacks();

    // 配置数据
    std::unordered_map<ActionId, ActionBinding> m_globalBindings;

    // 运行时状态
    std::unordered_map<ActionId, InputActionState> m_actionStates;
    std::unordered_map<ActionId, bool> m_prevFrameActive;

    // 回调注册表
    struct CallbackEntry {
        ActionCallbackId id;
        ActionId actionId;
        std::function<void(const InputActionState &)> callback;
        TriggerBehavior trigger;
    };
    std::vector<CallbackEntry> m_callbacks;
    ActionCallbackId m_nextCallbackId = 1;
};

} // namespace Input
} // namespace DX12Engine