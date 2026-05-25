#include "Bootstrap.h"
#include "Boot/ResourceConfig.h"
#include "ConfigManager.h"
#include "ECS/Core/Registry.h"
#include "Event/MessageDispatcher.h"
#include "GameContext.h"
#include "Logger/DebugOverlay.h"
#include "Logger/Logger.h"
#include "Platform/Input/InputManager.h"
#include "Platform/Windows/Window.h"
#include "Renderer/FrameResources/FrameResourceManager.h"
#include "Renderer/RHI/D3D12DeviceContext.h"
#include "Renderer/Scene/CameraManager.h"
#include "Resource/Core/DescriptorHeapCollection.h"
#include "Scheduler/FrameDriver.h"
#include <steam/isteamnetworkingutils.h>
#include <steam/steamnetworkingsockets.h>

using namespace DX12Engine::DebugUI;
using namespace DX12Engine::ECS;
using namespace DX12Engine::Scheduler;
using namespace DX12Engine::Input;
using namespace DX12Engine::Event;
using namespace DX12Engine::Renderer;
using EngineLogger = DX12Engine::Logger::Logger;
using namespace DX12Engine::Boot;
using namespace DX12Engine::Resource;

namespace {
void EarlyLog(const std::string &msg) {
    ::OutputDebugStringA(msg.c_str());
    ::OutputDebugStringA("\n");
    fprintf(stderr, "%s\n", msg.c_str());
}
} // namespace

namespace DX12Engine {

namespace Boot {

Bootstrap::~Bootstrap() { Shutdown(); }

void Bootstrap::Shutdown() {
    // 1. 关闭帧资源管理器
    m_frameResourceManager.Shutdown();

    // 2. 关闭描述符堆集合
    m_descriptorHeaps.Shutdown();

    // 1. 关闭调度器上下文 (FrameDriver)
    DX12Engine::Scheduler::ShutdownSchedulerContext();
    m_frameDriver = nullptr;

    // 2. 销毁 GameContext
    m_context.reset();

    // 3. 销毁 D3D12 设备上下文
    m_deviceContext.reset();

    // 4. 销毁 Window
    m_window.reset();

    // 5. 销毁 ECS Registry
    m_registry.reset();

    // 6. 关闭 MessageDispatcher 单例
    try {
        Event::MessageDispatcher::Shutdown();
    } catch (...) {
        // 忽略
    }

    // 7. 关闭 Logger (如果已初始化)
    // 注意：Logger::Shutdown 是静态方法，内部会处理单例清理
    try {
        EngineLogger::Shutdown();
    } catch (...) {
        // 忽略析构期间的异常
    }

    // 8. 关闭 ConfigManager
    try {
        ConfigManager::GetInstance().Shutdown();
    } catch (...) {
        // 忽略
    }

    // 9. 关闭 GNS 全局环境 (如果已初始化)
    // 注意：确保在所有网络实例销毁后调用
    GameNetworkingSockets_Kill();

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
    EngineLogger::GetInstance();

    // Logger::Init 内部会抛出异常如果失败
    EngineLogger::Init(logConfig);

    // 现在 Logger 可用了，后续日志可以使用 Logger
    EngineLogger::GetInstance()->Info("[Bootstrap] Logging system initialized.");
}

bool Bootstrap::CreateMainWindow() {
    EngineLogger::GetInstance()->Info("[Bootstrap] Creating Window...");

    const auto &windowConfig = ConfigManager::GetInstance().GetWindowConfig();

    m_window = std::make_unique<Platform::Window>(windowConfig);

    if (!m_window->Create()) {
        EngineLogger::GetInstance()->Error("[Bootstrap] Failed to create window");
        return false;
    }

    // SwapChain 创建前需要窗口已显示
    m_window->Show();

    EngineLogger::GetInstance()->Info("[Bootstrap] Window created successfully");
    return true;
}

bool Bootstrap::InitializeD3DDeviceContext() {
    EngineLogger::GetInstance()->Info("[Bootstrap] Initializing D3D12 Device Context...");

    // 获取窗口配置
    const auto &windowConfig = ConfigManager::GetInstance().GetWindowConfig();
    const auto &rendererConfig = ConfigManager::GetInstance().GetRendererConfig();

    // 配置 D3D12 设备上下文参数
    DX12Engine::Renderer::D3D12DeviceContext::InitParams params;

    params.hwnd = m_window->GetHandle();
    params.clientWidth = windowConfig.width;
    params.clientHeight = windowConfig.height;

    // 直接使用配置中已转换的枚举值
    params.backBufferFormat = rendererConfig.formats.BackBufferFormatEnum;
    params.depthStencilFormat = rendererConfig.formats.DepthStencilFormatEnum;

    params.enableDebugLayer = rendererConfig.device.enableDebugLayer;

    // MSAA 处理
    params.enable4xMsaa = rendererConfig.msaa.enabled && rendererConfig.msaa.sampleCount >= 4;

    // 使用已转换的 Feature Level
    params.minFeatureLevel = rendererConfig.device.FeatureLevelEnum;

    // 创建并初始化 D3D12 设备上下文
    m_deviceContext = std::make_unique<DX12Engine::Renderer::D3D12DeviceContext>();

    if (!m_deviceContext->Initialize(params)) {
        EngineLogger::GetInstance()->Error("[Bootstrap] Failed to initialize D3D12 Device Context");
        return false;
    }

    EngineLogger::GetInstance()->Info("[Bootstrap] D3D12 Device Context initialized successfully");
    return true;
}

void Bootstrap::InitializeRegistry() {
    EngineLogger::GetInstance()->Info("[Bootstrap] Initializing ECS Registry...");

    m_registry = std::make_unique<ECS::Registry>();

    EngineLogger::GetInstance()->Info("[Bootstrap] ECS Registry initialized successfully");
}

void Bootstrap::InitializeFrameDriver() {
    EngineLogger::GetInstance()->Info("[Bootstrap] Initializing FrameDriver...");

    if (!m_registry) {
        throw std::runtime_error("[Bootstrap] Registry must be initialized before FrameDriver");
    }

    if (!m_deviceContext) {
        throw std::runtime_error("[Bootstrap] D3D12DeviceContext must be initialized before FrameDriver");
    }

    // 创建全局调度器上下文，同时注入命令管理器
    ::DX12Engine::Scheduler::InitializeSchedulerContext(*m_registry, m_deviceContext.get());

    // 获取 FrameDriver 指针并保存
    auto &schedulerCtx = ::DX12Engine::Scheduler::GetSchedulerContext();
    if (!schedulerCtx.frameDriver) {
        throw std::runtime_error("[Bootstrap] Failed to create FrameDriver");
    }
    m_frameDriver = schedulerCtx.frameDriver;

    EngineLogger::GetInstance()->Info("[Bootstrap] FrameDriver initialized successfully");
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

        EngineLogger::GetInstance()->Info("[Bootstrap] Initializing InputManager...");
        auto &inputMgr = DX12Engine::Input::InputManager::Get();
        // 假设配置文件路径为 "Config/input_bindings.json" 或在 ConfigManager 中获取
        std::string inputConfigPath = "Config/default_input.json";

        if (inputMgr.Initialize(inputConfigPath)) {
            m_window->SetInputManager(&inputMgr); // ← 添加这行
        } else {
            EngineLogger::GetInstance()->Warn(
                "[Bootstrap] InputManager initialization failed or config not found. Using defaults.");
        }

        // 4. D3D12 设备上下文 (依赖窗口句柄)
        if (!InitializeD3DDeviceContext()) {
            throw std::runtime_error("[Bootstrap] InitializeD3DDeviceContext returned false.");
        }
        // ====================================================================
        // 初始化描述符堆集合
        // ====================================================================
        EngineLogger::GetInstance()->Info("[Bootstrap] Initializing DescriptorHeapCollection...");

        std::vector<Resource::DescriptorHeapConfig> heapConfigs = {
            // CBV_SRV_UAV 堆（大型，GPU 可见）
            {Resource::DescriptorHeapType::CbvSrvUav, 65536, 0,
             Resource::DescriptorSlotFlags::EnableExpand | Resource::DescriptorSlotFlags::DelayRelease, true},

            // RTV 堆（渲染目标，CPU 可见）
            {Resource::DescriptorHeapType::Rtv, 1024, 0,
             Resource::DescriptorSlotFlags::EnableExpand | Resource::DescriptorSlotFlags::DelayRelease, false},

            // DSV 堆（深度模板，CPU 可见）
            {Resource::DescriptorHeapType::Dsv, 512, 0,
             Resource::DescriptorSlotFlags::EnableExpand | Resource::DescriptorSlotFlags::DelayRelease, false},

            // Sampler 堆（固定 2048，GPU 可见）
            {Resource::DescriptorHeapType::Sampler, 2048, 2048, Resource::DescriptorSlotFlags::LinearAlloc, true}};

        m_descriptorHeaps.Initialize(m_deviceContext->GetDevice(), heapConfigs);
        EngineLogger::GetInstance()->Info("[Bootstrap] DescriptorHeapCollection initialized.");

        // ====================================================================
        // 初始化帧资源管理器
        // ====================================================================
        EngineLogger::GetInstance()->Info("[Bootstrap] Initializing FrameResourceManager...");

        m_frameResourceManager.Initialize(m_deviceContext->GetDevice(), &m_descriptorHeaps);

        EngineLogger::GetInstance()->Info("[Bootstrap] FrameResourceManager initialized.");

        InitializeDebugUI();

        // 5. MessageDispatcher 单例 (Event 层，调度系统需要)
        EngineLogger::GetInstance()->Info("[Bootstrap] Initializing MessageDispatcher...");
        Event::MessageDispatcher::Init();
        EngineLogger::GetInstance()->Info("[Bootstrap] MessageDispatcher initialized.");

        // 6. ECS Registry (调度系统需要)
        InitializeRegistry();

        // 7. FrameDriver (调度层核心，由基础设施层创建)
        InitializeFrameDriver();

        EngineLogger::GetInstance()->Info("[Bootstrap] Initializing GameNetworkingSockets...");
        SteamNetworkingErrMsg errMsg;
        if (!GameNetworkingSockets_Init(nullptr, errMsg)) {
            EngineLogger::GetInstance()->Error("[Bootstrap] GNS Init Failed: %s", errMsg);
            throw std::runtime_error("[Bootstrap] GNS Initialization Failed");
        }
        EngineLogger::GetInstance()->Info("[Bootstrap] GameNetworkingSockets initialized.");

    } catch (const std::exception &e) {
        // 如果任何一步失败，记录错误并重新抛出
        // 注意：如果 Logger 还没好，EarlyLog 会兜底
        std::string errMsg = std::string("[Bootstrap] Initialization failed: ") + e.what();

        // 尝试用 Logger 记录，如果 Logger 没好则用 EarlyLog
        try {
            if (EngineLogger::GetInstance()) { // 简单的空指针检查，具体取决于 Logger 实现
                EngineLogger::GetInstance()->Critical(errMsg.c_str());
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
    m_context->Logging = EngineLogger::GetInstance();
    m_context->Window = m_window.get();
    m_context->MainTimer = m_mainTimer.get();
    m_context->Dispatcher = Event::MessageDispatcher::GetInstance();
    m_context->Registry = m_registry.get();
    m_context->FrameDriver = m_frameDriver;
    m_context->DeviceContext = m_deviceContext.get();

    m_context->CameraMgr = &DX12Engine::Renderer::CameraManager::GetInstance();
    uint32_t width = m_window ? m_window->GetWidth() : 1280;
    uint32_t height = m_window ? m_window->GetHeight() : 720;
    m_context->CameraMgr->Initialize(width, height);

    m_context->DescriptorHeaps = &m_descriptorHeaps;
    m_context->FrameResourceManager = &m_frameResourceManager;

    m_context->InputMgr = &DX12Engine::Input::InputManager::Get();

    if (m_frameDriver) {
        m_frameDriver->SetGameContext(m_context.get());
    }

    auto &debugUI = DebugUI::DebugUIManager::Get();
    debugUI.SetGameContext(m_context.get());
    debugUI.AutoRegisterToFrameDriver(m_context.get());

    return m_context.get();
}

DX12Engine::ECS::Registry &Bootstrap::GetRegistry() {
    if (!m_registry) {
        throw std::runtime_error("[Bootstrap] Registry not initialized");
    }
    return *m_registry;
}

void Bootstrap::Run() {
    InitializeModules();
    m_isInitialized = true;
}

void Bootstrap::InitializeDebugUI() {
    EngineLogger::GetInstance()->Info("[Bootstrap] Initializing DebugUI...");

    auto &debugUI = DebugUI::DebugUIManager::Get();

    // 1. 初始化 Win32 后端
    debugUI.Initialize(m_window->GetHandle());

    // 2. 初始化 DX12 后端
    const auto &rendererConfig = ConfigManager::GetInstance().GetRendererConfig();

    debugUI.InitDX12Backend(m_deviceContext->GetDevice(), m_deviceContext->GetCommandQueue(), 2,
                            rendererConfig.formats.BackBufferFormatEnum);

    // 3. 可选：配置样式和默认行为
    debugUI.ApplyDarkTheme();
    debugUI.SetShowMenuBar(true);

    // 4. 注册到 FrameDriver（通过 GameContext）
    // 这一步在 CreateContext 中完成，因为需要 GameContext

    EngineLogger::GetInstance()->Info("[Bootstrap] DebugUI initialized successfully");
}

} // namespace Boot
} // namespace DX12Engine
