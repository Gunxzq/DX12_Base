#pragma once
#include "Core/InputActionId.h"
#include "Core/InputBinding.h"
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace DX12Engine {
namespace Input {

class InputContextStack {
public:
    void RegisterContexts(const std::unordered_map<std::string, InputContextConfig> &configs);

    void PushContext(const std::string &contextName);
    void PopContext();
    void Clear();

    const std::unordered_set<ActionId> &GetEnabledActions();

    bool IsActionEnabled(ActionId actionId);

    size_t GetStackDepth() const { return m_activeStack.size(); }

    std::string GetTopContext() const { return m_activeStack.empty() ? "" : m_activeStack.back(); }

#if defined(_DEBUG) || defined(DEVELOPMENT)
    // ========================================================================
    // 调试信息结构体
    // ========================================================================
    struct ContextDebugInfo {
        std::string Name;
        int Priority = 0;
        size_t EnabledActionCount = 0;
        bool IsActive = false; // 是否是当前栈顶
    };

    struct StackDebugInfo {
        std::vector<ContextDebugInfo> Contexts; // 所有上下文（从底到顶）
        std::vector<ActionId> EnabledActions;   // 当前启用的动作
        size_t TotalActionCount = 0;            // 总注册动作数
        size_t StackDepth = 0;
    };

    /// @brief 获取完整的调试信息（一次调用获取所有数据）
    StackDebugInfo GetDebugInfo() const {

        if (m_cacheDirty) {
            const_cast<InputContextStack *>(this)->RebuildCache();
        }

        StackDebugInfo info;
        info.StackDepth = m_activeStack.size();
        info.EnabledActions.assign(m_cachedEnabledActions.begin(), m_cachedEnabledActions.end());

        for (size_t i = 0; i < m_activeStack.size(); ++i) {
            const auto &ctxName = m_activeStack[i];
            auto it = m_contextConfigs.find(ctxName);
            if (it != m_contextConfigs.end()) {
                ContextDebugInfo ctxInfo;
                ctxInfo.Name = ctxName;
                ctxInfo.Priority = it->second.Priority;
                ctxInfo.EnabledActionCount = it->second.EnabledActions.size();
                ctxInfo.IsActive = (i == m_activeStack.size() - 1);
                info.Contexts.push_back(ctxInfo);
                info.TotalActionCount += ctxInfo.EnabledActionCount;
            }
        }

        return info;
    }

    /// @brief 简单获取栈内容（仅名称）
    std::vector<std::string> GetStackNames() const { return m_activeStack; }
#endif

private:
    void MarkDirty() { m_cacheDirty = true; }
    void RebuildCache();

    std::unordered_map<std::string, InputContextConfig> m_contextConfigs;
    std::vector<std::string> m_activeStack;
    std::unordered_set<ActionId> m_cachedEnabledActions;
    bool m_cacheDirty = true;
};

} // namespace Input
} // namespace DX12Engine