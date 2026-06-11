// InputManager.h
#pragma once

#include "Core/InputActionId.h"
#include "Core/InputKeyCodes.h"
#include "Math/MathTypes.h"

namespace DX12Engine {
namespace Input {

class RawInputBuffer;
class InputSystem;
class InputContextStack;

class InputManager {
public:
    static InputManager &Get();

    bool Initialize(const std::string &configPath, bool isModKeySeparated);

    // 上下文管理（组合生命周期）
    void PushContext(const std::string &contextName);
    void PopContext();
    void ClearContexts();

    // 窗口失焦时调用
    void ResetAllStates();

    void BeginFrame();                               // 清空增量数据，准备接收新消息
    void Update(float deltaTime, float currentTime); // 计算动作状态

    // 访问器方法
    InputSystem *GetInputSystem() { return m_inputSystem.get(); }
    RawInputBuffer *GetRawBuffer() { return m_rawBuffer.get(); }
    InputContextStack *GetContextStack() { return m_contextStack.get(); }

private:
    InputManager() = default;
    ~InputManager() = default;

    std::unique_ptr<RawInputBuffer> m_rawBuffer;
    std::unique_ptr<InputSystem> m_inputSystem;
    std::unique_ptr<InputContextStack> m_contextStack;
};

} // namespace Input
} // namespace DX12Engine