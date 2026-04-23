#include "Core/Bootstrap/Bootstrap.h"
#include "Core/Config/ConfigManager.h"
#include "Core/Context/GameContext.h"
#include "Renderer/Core/D3D12DeviceContext.h"
#include "Runtime/Scene/Camera.h"
#include "System/Logger/DebugOverlay.h"
#include "System/Logger/Logger.h"
#include "System/Window/Window.h"

namespace {
void EarlyLog(const std::string &msg) {
    ::OutputDebugStringA(msg.c_str());
    ::OutputDebugStringA("\n");
    fprintf(stderr, "%s\n", msg.c_str());
}
} // namespace

namespace DX12Engine {
namespace Core {

Bootstrap::~Bootstrap() { Shutdown(); }

void Bootstrap::Shutdown() {
    // 1. 销毁 GameContext
    m_context.reset();

    // 2. 销毁 D3D12 设备上下文
    m_deviceContext.reset();

    // 3. 销毁 Window
    m_window.reset();

    // 4. 关闭 Logger (如果已初始化)
    // 注意：Logger::Shutdown 是静态方法，内部会处理单例清理
    try {
        Logger::Shutdown();
    } catch (...) {
        // 忽略析构期间的异常
    }

    // 5. 关闭 ConfigManager
    try {
        ConfigManager::GetInstance().Shutdown();
    } catch (...) {
        // 忽略
    }

    m_isInitialized = false;
}

void Bootstrap::InitializeConfigManager(const std::filesystem::path &configDir) {
    EarlyLog("[Bootstrap] Initializing ConfigManager...");

    ConfigManager::GetInstance().Initialize(configDir);

    EarlyLog("[Bootstrap] ConfigManager initialized.");
}

void Bootstrap::InitializeLogging() {
    EarlyLog("[Bootstrap] Initializing Logging...");

    // 获取配置 (此时 ConfigManager 必须已初始化)
    const auto &logConfig = ConfigManager::GetInstance().GetLogConfig();

    // 任何获取实例，迫使创建
    Logger::GetInstance();

    // Logger::Init 内部会抛出异常如果失败
    Logger::Init(logConfig);

    // 现在 Logger 可用了，后续日志可以使用 Logger
    Logger::GetInstance()->Info("[Bootstrap] Logging system initialized.");
}

bool Bootstrap::CreateMainWindow() {
    Logger::GetInstance()->Info("[Bootstrap] Creating Window...");

    const auto &windowConfig = ConfigManager::GetInstance().GetWindowConfig();

    m_window = std::make_unique<Window>(windowConfig);

    if (!m_window->Create()) {
        Logger::GetInstance()->Error("[Bootstrap] Failed to create window");
        return false;
    }

    Logger::GetInstance()->Info("[Bootstrap] Window created successfully");
    return true;
}

bool Bootstrap::InitializeD3DDeviceContext() {
    Logger::GetInstance()->Info("[Bootstrap] Initializing D3D12 Device Context...");

    // 获取窗口配置
    const auto &windowConfig = ConfigManager::GetInstance().GetWindowConfig();

    // 配置 D3D12 设备上下文参数
    DX12Engine::Renderer::D3D12DeviceContext::InitParams params;
    params.hwnd = m_window->GetHandle();
    params.clientWidth = windowConfig.width;
    params.clientHeight = windowConfig.height;
    params.backBufferFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
    params.depthStencilFormat = DXGI_FORMAT_D24_UNORM_S8_UINT;
    params.enableDebugLayer = true; // 默认启用调试层
    params.enable4xMsaa = false;    // 默认禁用 MSAA
    params.minFeatureLevel = D3D_FEATURE_LEVEL_11_0;

    // 创建并初始化 D3D12 设备上下文
    m_deviceContext = std::make_unique<DX12Engine::Renderer::D3D12DeviceContext>();

    if (!m_deviceContext->Initialize(params)) {
        Logger::GetInstance()->Error("[Bootstrap] Failed to initialize D3D12 Device Context");
        return false;
    }

    Logger::GetInstance()->Info("[Bootstrap] D3D12 Device Context initialized successfully");
    return true;
}

void Bootstrap::InitializeModules() {
    try {
        // 1. 配置 (基础)
        InitializeConfigManager("Config");

        // 2. 日志 (依赖配置)
        InitializeLogging();

        // 3. 窗口 (依赖配置)
        if (!CreateMainWindow()) {
            throw std::runtime_error("[Bootstrap] CreateMainWindow returned false.");
        }

        // 4. D3D12 设备上下文 (依赖窗口句柄)
        if (!InitializeD3DDeviceContext()) {
            throw std::runtime_error("[Bootstrap] InitializeD3DDeviceContext returned false.");
        }

    } catch (const std::exception &e) {
        // 如果任何一步失败，记录错误并重新抛出
        // 注意：如果 Logger 还没好，EarlyLog 会兜底
        std::string errMsg = std::string("[Bootstrap] Initialization failed: ") + e.what();

        // 尝试用 Logger 记录，如果 Logger 没好则用 EarlyLog
        try {
            if (Logger::GetInstance()) { // 简单的空指针检查，具体取决于 Logger 实现
                Logger::GetInstance()->Critical(errMsg.c_str());
            } else {
                EarlyLog(errMsg);
            }
        } catch (...) {
            EarlyLog(errMsg);
        }

        // 清理已初始化的部分
        Shutdown();

        // 重新抛出，让 main 函数决定如何处理（如弹窗）
        throw;
    }
}

GameContext *Bootstrap::CreateContext() {
    if (!m_isInitialized) {
        throw std::runtime_error("[Bootstrap] Cannot create context: Bootstrap not initialized.");
    }

    // 创建主计时器（在窗口和配置之后）
    m_mainTimer = std::make_unique<GameTimer>();

    m_context = std::make_unique<GameContext>();
    m_context->Config = &ConfigManager::GetInstance();
    m_context->Logging = Logger::GetInstance();
    m_context->Window = m_window.get();
    m_context->MainTimer = m_mainTimer.get();
    m_context->DeviceContext = m_deviceContext.get();

    return m_context.get();
}

void Bootstrap::Run() {
    InitializeModules();
    m_isInitialized = true;
}

} // namespace Core
} // namespace DX12Engine
