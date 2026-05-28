#include "InputContextStack.h"

#include "Common/Common.h"

namespace DX12Engine {
namespace Input {

void InputContextStack::RegisterContexts(const std::unordered_map<std::string, InputContextConfig> &configs) {
    m_contextConfigs = configs;
    MarkDirty();
}

void InputContextStack::PushContext(const std::string &contextName) {
    if (m_contextConfigs.find(contextName) != m_contextConfigs.end()) {
        m_activeStack.push_back(contextName);
        MarkDirty();
    }
}

/**
 * @brief 弹出当前栈顶上下文
 * @date 2026-05-22
 */
void InputContextStack::PopContext() {
    if (m_activeStack.size() > 1) { // 至少保留一个上下文，避免栈空
        m_activeStack.pop_back();
        MarkDirty();
    }
}

/**
 * @brief 清空当前上下文栈
 * @date 2026-05-22
 */
void InputContextStack::Clear() {
    if (m_activeStack.size() > 1) {
        auto bottom = m_activeStack.front();
        m_activeStack.clear();
        m_activeStack.push_back(bottom);
        MarkDirty();
    }
}

/**
 * @brief 获取当前启用的动作集合（自动重建缓存）
 * @return const std::unordered_set<ActionId>&
 * @date 2026-05-22
 */
const std::unordered_set<ActionId> &InputContextStack::GetEnabledActions() {
    if (m_cacheDirty) {
        RebuildCache(); // 重建缓存
    }
    return m_cachedEnabledActions;
}

/**
 * @brief 检查指定动作是否已启用
 * @param actionId 动作ID
 * @return bool
 * @date 2026-05-22
 */
bool InputContextStack::IsActionEnabled(ActionId actionId) {
    const auto &enabled = GetEnabledActions();
    return enabled.find(actionId) != enabled.end();
}

/**
 * @brief 重建启用动作缓存
 * @date 2026-05-22
 */
void InputContextStack::RebuildCache() {
    m_cachedEnabledActions.clear();

    if (m_activeStack.empty())
        return;

    // 只取栈顶上下文
    const std::string &topContext = m_activeStack.back();
    auto found = m_contextConfigs.find(topContext);
    if (found != m_contextConfigs.end()) {
        const auto &ctx = found->second;
        m_cachedEnabledActions.insert(ctx.EnabledActions.begin(), ctx.EnabledActions.end());
    }

    m_cacheDirty = false;
}

} // namespace Input
} // namespace DX12Engine