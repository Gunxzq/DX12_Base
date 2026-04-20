#include "Core/Bootstrap/Bootstrap.h"

#include "Core/Config/ConfigManager.h"
#include "Core/Window/Window.h"

namespace DX12Engine {
namespace Core {

bool Bootstrap::Initialize(const std::filesystem::path &configDir) {

    // 1. 初始化配置管理器
    m_configManager = std::make_unique<ConfigManager>();
    m_configManager->Initialize(configDir);

    // 2. 获取窗口配置
    Window::Desc windowDesc = GetWindowDesc();

    // 3. 创建窗口
    m_window = std::make_unique<Window>(windowDesc);
    if (!m_window->Create()) {
        return false;
    }

    return true;
}

void Bootstrap::Run() {
    if (!m_window) {
        return;
    }

    m_isRunning = true;

    // 主消息循环
    while (m_isRunning && !m_window->ShouldClose()) {
        // 处理 Windows 消息
        m_window->ProcessMessages();
    }
}

void Bootstrap::Shutdown() {
    m_isRunning = false;

    // 关闭配置管理器
    if (m_configManager) {
        m_configManager->Shutdown();
        m_configManager.reset();
    }

    // 销毁窗口
    m_window.reset();
}

Window::Desc Bootstrap::GetWindowDesc() const {
    Window::Desc desc;

    if (!m_configManager) {
        return desc; // 返回默认值
    }

    // 从配置管理器获取窗口配置
    const auto &windowConfig = m_configManager->GetWindowConfig();

    desc.title = windowConfig.title;
    desc.width = windowConfig.width;
    desc.height = windowConfig.height;
    desc.resizable = windowConfig.resizable;

    return desc;
}

} // namespace Core
} // namespace DX12Engine
