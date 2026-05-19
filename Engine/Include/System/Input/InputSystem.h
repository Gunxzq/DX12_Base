#pragma once
#include "Math/MathTypes.h"
#include "System/Input/Core/InputActionId.h"
#include "System/Input/Core/InputActionState.h"
#include "System/Input/Core/InputBinding.h"
#include "System/Input/Core/InputKeyCodes.h"
#include "System/Input/InputContextStack.h"
#include <memory>
#include <unordered_map>

// 前置声明
namespace DX12Engine {
namespace Input {
class RawInputBuffer;
} // namespace Input
} // namespace DX12Engine

namespace DX12Engine {
namespace Input {

class InputSystem {
public:
    static InputSystem &Get();

    // --- 初始化与加载 ---
    bool Initialize(const std::string &configPath);

    // --- 每帧更新 ---
    // rawBuffer: 当前帧采集到的原始硬件状态
    void Update(RawInputBuffer &rawBuffer, float deltaTime, float currentTime);

    // --- 上下文管理 ---
    void PushContext(const std::string &contextName) { m_contextStack.PushContext(contextName); }
    void PopContext() { m_contextStack.PopContext(); }

    // --- 查询接口 (Game Layer 使用) ---
    const InputActionState &GetActionState(ActionId actionId) const;

    // 便捷查询
    bool IsActionPressed(ActionId actionId) const { return GetActionState(actionId).GetPressed(); }

    bool IsActionReleased(ActionId actionId) const { return GetActionState(actionId).GetReleased(); }

    bool IsActionHeld(ActionId actionId) const { return GetActionState(actionId).GetHeld(); }

    float GetActionValue1D(ActionId actionId) const { return GetActionState(actionId).GetValue1D(); }

    FVector2D GetActionAxis2D(ActionId actionId) const; // 需定义 FVector2D 或使用 std::pair

private:
    InputSystem() = default;

    // 内部状态计算逻辑
    void EvaluateActions(RawInputBuffer &rawBuffer, float currentTime);

    bool IsKeyboardKey(EKeyCode code);

    // 数据成员
    std::unordered_map<ActionId, ActionBinding> m_globalBindings;
    std::unordered_map<std::string, InputContextConfig> m_contextConfigs;
    std::unordered_map<ActionId, InputActionState> m_actionStates; // 缓存每帧结果

    InputContextStack m_contextStack;

    // 用于检测 Pressed/Released 的上一帧状态
    std::unordered_map<ActionId, bool> m_prevFrameDigitalState;
};

} // namespace Input
} // namespace DX12Engine