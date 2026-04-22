// 取消定义 Windows API 宏，避免与方法名冲突
#ifdef CreateWindow
#undef CreateWindow
#endif
#ifdef GetWindowLong
#undef GetWindowLong
#endif
#ifdef GetWindowText
#undef GetWindowText
#endif
#ifdef SetWindowText
#undef SetWindowText
#endif

#include "Core/Bootstrap/Bootstrap.h"
#include "Core/Config/ConfigManager.h"
#include "Core/Context/GameContext.h"
#include "System/Logger/DebugOverlay.h"
#include "System/Logger/Logger.h"
#include "System/Window/Window.h"

namespace DX12Engine {
namespace Core {

Bootstrap::~Bootstrap() { Shutdown(); }

void Bootstrap::Shutdown() {}

void Bootstrap::InitializeConfigManager(const std::filesystem::path &configDir) {
    Logger::GetInstance()->Info("[Bootstrap] Initializing ConfigManager...");
    ConfigManager::GetInstance().Initialize(configDir);
}

void Bootstrap::InitializeLogging() {
    Logger::GetInstance()->Info("[Bootstrap] Initializing Logging...");
    const auto &logConfig = ConfigManager::GetInstance().GetLogConfig();
    Logger::Init(logConfig);
}

bool Bootstrap::CreateMainWindow() {
    Logger::GetInstance()->Info("[Bootstrap] Creating Window...");

    Window::Desc desc;
    {
        const auto &windowConfig = ConfigManager::GetInstance().GetWindowConfig();
        desc.title = windowConfig.title;
        desc.width = windowConfig.width;
        desc.height = windowConfig.height;
        desc.resizable = windowConfig.resizable;
    }
    m_window = std::make_unique<Window>(desc);

    if (!m_window->Create()) {
        Logger::GetInstance()->Error("[Bootstrap] Failed to create window");
        return false;
    }

    Logger::GetInstance()->Info("[Bootstrap] Window created successfully");
    return true;
}

void Bootstrap::InitializeModules() {
    InitializeConfigManager("Config");
    InitializeLogging();
    CreateMainWindow();
}

GameContext *Bootstrap::CreateContext() {
    m_context = std::make_unique<GameContext>();
    m_context->Config = &ConfigManager::GetInstance();
    m_context->Logging = Logger::GetInstance();
    m_context->Window = m_window.get();

    return m_context.get();
}

void Bootstrap::Run() {
    InitializeModules();
    m_isInitialized = true;
}

} // namespace Core
} // namespace DX12Engine
