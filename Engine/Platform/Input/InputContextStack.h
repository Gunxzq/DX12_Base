#pragma once
#include "Core/InputBinding.h"
#include <string>
#include <unordered_map>
#include <vector>

namespace DX12Engine {
namespace Input {

class InputContextStack {
public:
    // 注册所有可用的上下文配置（从 JSON 加载后调用）
    void RegisterContexts(const std::unordered_map<std::string, InputContextConfig> &configs) {
        m_contextConfigs = configs;
    }

    // 压入一个新上下文（例如打开 UI 时 Push("UI")）
    void PushContext(const std::string &contextName) {
        if (m_contextConfigs.find(contextName) != m_contextConfigs.end()) {
            m_activeStack.push_back(contextName);
        }
    }

    // 弹出当前上下文
    void PopContext() {
        if (!m_activeStack.empty()) {
            m_activeStack.pop_back();
        }
    }

    // 获取当前所有活跃上下文的合并配置
    // 返回一个有序的列表，优先级从高到低
    std::vector<const InputContextConfig *> GetActiveContexts() const {
        std::vector<const InputContextConfig *> result;
        // 从栈顶往栈底遍历，确保高优先级在前
        for (auto it = m_activeStack.rbegin(); it != m_activeStack.rend(); ++it) {
            auto found = m_contextConfigs.find(*it);
            if (found != m_contextConfigs.end()) {
                result.push_back(&found->second);
            }
        }
        return result;
    }

    // 清空栈（例如游戏结束时）
    void Clear() { m_activeStack.clear(); }

private:
    std::unordered_map<std::string, InputContextConfig> m_contextConfigs;
    std::vector<std::string> m_activeStack; // 栈底是 Gameplay，栈顶是 UI
};

} // namespace Input
} // namespace DX12Engine