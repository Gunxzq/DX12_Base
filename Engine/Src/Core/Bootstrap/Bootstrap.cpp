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
#include "Core/DebugOverlay/LogWindow.h"
#include "Core/Game/Game.h"
#include "Core/Logger/Logger.h"
#include "Core/Window/Window.h"

namespace DX12Engine {
namespace Core {

Bootstrap::~Bootstrap() { Shutdown(); }

void Bootstrap::Shutdown() {
    Logger::GetInstance()->Info("[Bootstrap] Shutting down...");

    // 清理顺序：Game -> Context -> Window -> LogWindow
    m_game.reset();
    m_context.reset();
    m_window.reset();

    // 销毁 LogWindow
    if (LogWindow::GetInstance()) {
        delete LogWindow::GetInstance();
    }

    // ConfigManager 是单例，调用 Shutdown 清理
    ConfigManager::GetInstance().Shutdown();

    m_isInitialized = false;

    Logger::GetInstance()->Info("[Bootstrap] Shutdown complete");

    // Logger 是单例，最后关闭
    Logger::Shutdown();
}

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
    // 注意：初始化顺序很重要
    // 1. ConfigManager 必须最先初始化（其他模块依赖配置）
    // 2. Logger 需要从 ConfigManager 获取配置
    // 3. Window 最后创建（依赖 ConfigManager 的窗口配置）
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

    m_game = std::make_unique<Game>(CreateContext());

    if (!m_game->Initialize()) {
        Logger::GetInstance()->Error("[Bootstrap] Failed to initialize Game");
        Shutdown(); // 确保日志被刷新
        return;
    }

    m_isInitialized = true;

    m_game->Run();
}

} // namespace Core
} // namespace DX12Engine
