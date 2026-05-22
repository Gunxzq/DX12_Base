#pragma once
#include "Core/InputActionId.h"
#include "Core/InputActionState.h"
#include "Core/InputBinding.h"
#include "Core/InputKeyCodes.h"
#include "Math/MathTypes.h"
#include <unordered_map>
#include <unordered_set>

// 前置声明
namespace DX12Engine {
namespace Input {
class RawInputBuffer;

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

    void ResetAllStates();

private:
    void EvaluateActions(const RawInputBuffer &rawBuffer, const std::unordered_set<ActionId> &enabledActions,
                         float currentTime);
    void PerformEdgeDetection(float currentTime);
    void SavePreviousFrameState();
    void ResetCurrentFrameState();

    // 配置数据
    std::unordered_map<ActionId, ActionBinding> m_globalBindings;

    // 运行时状态
    std::unordered_map<ActionId, InputActionState> m_actionStates;
    std::unordered_map<ActionId, bool> m_prevFrameActive;
};

} // namespace Input
} // namespace DX12Engine