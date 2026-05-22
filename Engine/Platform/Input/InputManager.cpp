// InputManager.cpp
#include "InputManager.h"
#include "InputConfigLoader.cpp"
#include "InputContextStack.h"
#include "InputSystem.h"
#include "RawInputBuffer.h"

namespace DX12Engine::Input {

InputManager &InputManager::Get() {
    static InputManager instance;
    return instance;
}

bool InputManager::Initialize(const std::string &configPath) {
    // 创建子模块
    m_rawBuffer = std::make_unique<RawInputBuffer>();
    m_inputSystem = std::make_unique<InputSystem>();
    m_contextStack = std::make_unique<InputContextStack>();

    // 加载配置
    std::unordered_map<ActionId, ActionBinding> globalBindings;
    std::unordered_map<std::string, InputContextConfig> contextConfigs;

    bool success = InputConfigLoader::LoadConfig(configPath, globalBindings, contextConfigs);
    if (!success)
        return false;

    // 分发配置
    m_inputSystem->SetGlobalBindings(std::move(globalBindings));
    m_contextStack->RegisterContexts(contextConfigs);
    m_contextStack->PushContext("Gameplay");

    return true;
}

void InputManager::PushContext(const std::string &contextName) { m_contextStack->PushContext(contextName); }

void InputManager::PopContext() {
    m_contextStack->PopContext();
    // 弹出后重置缓冲区，避免残留输入
    m_rawBuffer->Reset();
    m_inputSystem->ResetAllStates();
}

void InputManager::ClearContexts() {
    m_contextStack->Clear();
    m_rawBuffer->Reset();
    m_inputSystem->ResetAllStates();
}

void InputManager::ResetAllStates() {
    m_rawBuffer->Reset();
    m_inputSystem->ResetAllStates();
}

void InputManager::BeginFrame() { m_rawBuffer->BeginFrame(); }

void InputManager::Update(float deltaTime, float currentTime) {
    const auto &enabledActions = m_contextStack->GetEnabledActions();
    m_inputSystem->Update(*m_rawBuffer, enabledActions, deltaTime, currentTime);
}

} // namespace DX12Engine::Input