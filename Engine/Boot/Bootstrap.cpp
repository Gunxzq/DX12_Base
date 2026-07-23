#include "Bootstrap.h"

#include "Common/Common.h"

#include "Asset/IO/AssetLoader.h"
#include "Core/Config/ConfigManager.h"
#include "Core/Config/ConfigTypes/ResourceConfig.h"
#include "Core/SharedDataStore/SharedDataStore.h"
#include "ECS/Core/Registry.h"
#include "ECS/World.h"
#include "Event/MessageDispatcher.h"
#include "GameContext.h"
#include "Logger/Logger.h"
#include "Platform/Input/InputManager.h"
#include "Platform/Windows/Window.h"
#include "Renderer/FrameResources/FrameResourceConfig.h"
#include "Renderer/FrameResources/FrameResourceManager.h"
#include "Renderer/RHI/D3D12DeviceContext.h"
#include "Renderer/Scene/CameraManager.h"
#include "Renderer/Scene/LightManager/LightManager.h"
#include "Renderer/Utils/ShaderUtils.h"
#include "Resource/AssetManager/AssetManager.h"
#include "Resource/Core/DescriptorHeapCollection.h"
#include "Resource/GpuResourceManager.h"
#include "Resource/Pool/DepthStencilPool.h"
#include "Resource/Pool/RenderTargetPool.h"
#include "Scene/SceneManager.h"
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

    // 3. 关闭 FrameDriver（由 Bootstrap 直接管理）
    m_frameDriver.reset();

    // 2. 销毁 GameContext
    m_context.reset();

    // 2.5 关闭深度模板资源池
    try {
        DepthStencilPool::GetInstance().Shutdown();
    } catch (...) {
        // 忽略
    }

    // 2.6 关闭渲染目标资源池
    try {
        RenderTargetPool::GetInstance().Shutdown();
    } catch (...) {
        // 忽略
    }

    // 2.7 关闭 AssetDataManager
    try {
        Core::SharedDataStore::GetInstance().Shutdown();
    } catch (...) {
        // 忽略
    }

    // 3. 销毁 D3D12 设备上下文
    m_deviceContext.reset();

    // 4. 销毁 Window
    m_window.reset();

    // 5. 关闭 World（ECS 绝对源头，在 SceneManager 之后销毁）
    m_world.Shutdown();
    m_sceneManager.Shutdown();

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

    auto &cfg = ConfigManager::GetInstance();
    cfg.Initialize(configDir);

    // 注册引擎 CORE 配置（Register 时立即加载 + apply）
    cfg.Register("renderer", {configDir / "renderer.json", ConfigManager::ConfigFormat::JSON, true},
                 [this](const nlohmann::json &j) {
                     if (!j.is_null() && j.contains("renderer")) {
                         m_rendererConfig = j["renderer"].get<RendererConfig>();
                     }
                     m_rendererConfig.PostLoad();
                 });

    cfg.Register("window", {configDir / "window.ini", ConfigManager::ConfigFormat::INI, false},
                 [this](const nlohmann::json &j) {
                     if (j.is_null() || !j.contains("window")) {
                         ErrorReporter::Fatal("Config/window.ini: 缺少 [window] 节\n"
                                              "期望格式:\n"
                                              "[window]\n"
                                              "title=DX12 Engine\n"
                                              "width=1920\n"
                                              "height=1080\n"
                                              "mode=windowed\n"
                                              "resizable=true\n"
                                              "maximizable=true\n");
                     }
                     auto &w = j["window"];
                     if (w.contains("title") && w["title"].is_string()) {
                         std::string t = w["title"];
                         m_windowConfig.title.assign(t.begin(), t.end());
                     }
                     if (w.contains("width") && w["width"].is_number_unsigned())
                         m_windowConfig.width = w["width"].get<uint32_t>();
                     if (w.contains("height") && w["height"].is_number_unsigned())
                         m_windowConfig.height = w["height"].get<uint32_t>();
                     if (w.contains("mode") && w["mode"].is_string())
                         m_windowConfig.mode = w["mode"].get<std::string>();
                     if (w.contains("resizable") && w["resizable"].is_boolean())
                         m_windowConfig.resizable = w["resizable"].get<bool>();
                     if (w.contains("maximizable") && w["maximizable"].is_boolean())
                         m_windowConfig.maximizable = w["maximizable"].get<bool>();
                     if (w.contains("inputPriorityIsImGuiFirst") && w["inputPriorityIsImGuiFirst"].is_boolean())
                         m_windowConfig.inputPriorityIsImGuiFirst = w["inputPriorityIsImGuiFirst"].get<bool>();
                 });

    cfg.Register("logging", {configDir / "logging_config.json", ConfigManager::ConfigFormat::JSON, true},
                 [this](const nlohmann::json &j) {
                     if (!j.is_null() && j.contains("logging")) {
                         m_logConfig = j["logging"].get<LogConfig>();
                     }
                 });

    EarlyLog("[Bootstrap] ConfigManager initialized.");
}

void Bootstrap::InitializeLogging() {
    EarlyLog("[Bootstrap] Initializing Logging...");

    // 获取配置 (此时 ConfigManager 必须已初始化)
    const auto &logConfig = m_logConfig;

    // 任何获取实例，迫使创建
    EngineLogger::GetInstance();

    // Logger::Init 内部会抛出异常如果失败
    EngineLogger::Init(logConfig);

    // 现在 Logger 可用了，后续日志可以使用 Logger
    EngineLogger::GetInstance()->Info("[Bootstrap] Logging system initialized.");
}

bool Bootstrap::CreateMainWindow() {
    EngineLogger::GetInstance()->Info("[Bootstrap] Creating Window...");

    const auto &windowConfig = m_windowConfig;

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
    const auto &windowConfig = m_windowConfig;
    const auto &rendererConfig = m_rendererConfig;

    // 配置 D3D12 设备上下文参数
    DX12Engine::Renderer::D3D12DeviceContext::InitParams params;

    params.hwnd = m_window->GetHandle();
    params.clientWidth = windowConfig.width;
    params.clientHeight = windowConfig.height;
    // params.adapterIndex = 1; // 强制使用 NVIDIA GeForce MX330

    // 直接使用配置中已转换的枚举值
    params.backBufferFormat = rendererConfig.formats.BackBufferFormatEnum;
    params.depthStencilFormat = rendererConfig.formats.DepthStencilFormatEnum;

    params.enableDebugLayer = rendererConfig.device.enableDebugLayer;
    params.enableGPUBasedValidation = rendererConfig.device.enableGPUBasedValidation;

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

void Bootstrap::InitializeFrameDriver() {
    EngineLogger::GetInstance()->Info("[Bootstrap] Initializing FrameDriver...");

    if (!m_deviceContext) {
        throw std::runtime_error("[Bootstrap] D3D12DeviceContext must be initialized before FrameDriver");
    }

    // 直接创建 FrameDriver（不再通过 SchedulerContext）
    m_frameDriver = std::make_unique<Scheduler::FrameDriver>();
    m_frameDriver->SetDeviceContext(m_deviceContext.get());
    m_frameDriver->Initialize();

    EngineLogger::GetInstance()->Info("[Bootstrap] FrameDriver initialized successfully");
}

void Bootstrap::InitializeModules() {
    try {
        // 0. 项目路径（在所有模块之前初始化）
        EarlyLog("[Bootstrap] Project root: " + m_projectConfig.Root);

        // 设置着色器根目录（ShaderUtils 全局缓存）
        // 注意：传入项目根目录，因为渲染器的文件名包含 "Shaders/" 前缀
        Renderer::SetShaderRoot(m_projectConfig.Root);

        // 1. 配置 (基础) — ConfigRoot 是相对项目根的路径，拼接为绝对路径
        auto configDir = (std::filesystem::path(m_projectConfig.Root) / m_projectConfig.ConfigRoot).string();
        InitializeConfigManager(configDir);

        // 2. 日志 (依赖配置)
        InitializeLogging();

        // 日志就绪后挂载到 ErrorReporter
        ErrorReporter::SetLogger(EngineLogger::GetInstance());

        EngineLogger::GetInstance()->Info("[Bootstrap] Initializing InputManager...");
        auto &inputMgr = DX12Engine::Input::InputManager::Get();
        // 假设配置文件路径为 "Config/input_bindings.json" 或在 ConfigManager 中获取
        std::string inputConfigPath = "Config/default_input.json";

        inputMgr.Initialize(inputConfigPath, false);

        // 3. 窗口 (依赖配置)
        if (!CreateMainWindow()) {
            throw std::runtime_error("[Bootstrap] CreateMainWindow returned false.");
        }

        m_window->SetInputManager(&inputMgr);
        m_window->SetInputPriority(m_windowConfig.inputPriorityIsImGuiFirst);
        EngineLogger::GetInstance()->Info("[Bootstrap] Window inputPriorityIsImGuiFirst={}",
                                          m_windowConfig.inputPriorityIsImGuiFirst);

        // 4. D3D12 设备上下文 (依赖窗口句柄)
        if (!InitializeD3DDeviceContext()) {
            throw std::runtime_error("[Bootstrap] InitializeD3DDeviceContext returned false.");
        }
        // ====================================================================
        // 初始化描述符堆集合
        // ====================================================================
        EngineLogger::GetInstance()->Info("[Bootstrap] Initializing DescriptorHeapCollection...");

        std::vector<Resource::DescriptorHeapConfig> heapConfigs = {
            // CBV_SRV_UAV 堆（大型，GPU 可见；为 Texture/Buffer/Shadow 分区预留空间）
            {D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 131072, 0,
             Resource::DescriptorSlotFlags::EnableExpand | Resource::DescriptorSlotFlags::DelayRelease, true},

            // RTV 堆（渲染目标，CPU 可见）
            {D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1024, 0,
             Resource::DescriptorSlotFlags::EnableExpand | Resource::DescriptorSlotFlags::DelayRelease, false},

            // DSV 堆（深度模板，CPU 可见）
            {D3D12_DESCRIPTOR_HEAP_TYPE_DSV, 512, 0,
             Resource::DescriptorSlotFlags::EnableExpand | Resource::DescriptorSlotFlags::DelayRelease, false},

            // Sampler 堆（固定 2048，GPU 可见）
            {D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER, 2048, 2048, Resource::DescriptorSlotFlags::LinearAlloc, true}};

        bool isEditor = (m_projectConfig.Type == "editor");

        m_descriptorHeaps.Initialize(m_deviceContext->GetDevice(), heapConfigs,
                                     isEditor ? Resource::HeapMode::Multi : Resource::HeapMode::Single);
        EngineLogger::GetInstance()->Info("[Bootstrap] DescriptorHeapCollection initialized.");

        // 初始化 GPU 资源管理器（全局分配器，Game/Editor 共用）
        Resource::GpuResourceManager::GetInstance().Initialize();

        // 将描述符堆集合挂到 D3D12DeviceContext（供深度 SRV 等惰性创建使用）
        m_deviceContext->SetDescriptorHeapCollection(&m_descriptorHeaps);

        // 创建 Texture 分区（纹理 SRV，gTextureMaps[] 无界表使用）
        m_descriptorHeaps.AddPartition(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, Resource::PartitionType::Texture, 0,
                                       16384);
        EngineLogger::GetInstance()->Info("[Bootstrap] Texture partition created: base=0, size=16384");

        // 创建 Buffer 分区（MaterialBuffer、InstanceData 等 StructuredBuffer SRV）
        m_descriptorHeaps.AddPartition(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, Resource::PartitionType::Buffer, 16384,
                                       81920);
        EngineLogger::GetInstance()->Info("[Bootstrap] Buffer partition created: base=16384, size=81920");

        // 创建 Shadow 分区（阴影贴图 SRV，gShadowMaps[] 无界数组使用）
        // 包含方向光阴影 + 点光源 6×面 + 聚光灯 + 未来 CSM 级联
        m_descriptorHeaps.AddPartition(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, Resource::PartitionType::Shadow, 98304,
                                       1024);
        EngineLogger::GetInstance()->Info("[Bootstrap] Shadow partition created: base=98304, size=1024");

        // 多堆模式下为每个非 Default 标签创建相同的分区（ImGui 除外，它有专用小堆）
        if (isEditor && m_descriptorHeaps.GetMode() == Resource::HeapMode::Multi) {
            for (uint32_t t = static_cast<uint32_t>(Resource::HeapTag::Default) + 1;
                 t < static_cast<uint32_t>(Resource::HeapTag::Count); ++t) {
                auto tag = static_cast<Resource::HeapTag>(t);
                if (tag == Resource::HeapTag::ImGui)
                    continue; // ImGui 使用下面的专用小堆
                m_descriptorHeaps.AddPartition(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, Resource::PartitionType::Texture,
                                               0, 16384, tag);
                m_descriptorHeaps.AddPartition(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, Resource::PartitionType::Buffer,
                                               16384, 81920, tag);
                m_descriptorHeaps.AddPartition(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, Resource::PartitionType::Shadow,
                                               98304, 1024, tag);
                EngineLogger::GetInstance()->Info("[Bootstrap] Partitions created for HeapTag::%s",
                                                  Resource::HeapTagToString(tag));
            }
        }

        // ── HeapTag::ImGui 专用小堆 ──
        // 在 Multi 模式下创建独立物理堆，Single 模式下 share Default 堆
        {
            if (m_descriptorHeaps.GetMode() == Resource::HeapMode::Multi) {
                // 多堆：创建独立的 2048 槽 CBV_SRV_UAV 堆
                std::vector<Resource::DescriptorHeapConfig> imguiConfig = {
                    {D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 2048, 2048, Resource::DescriptorSlotFlags::LinearAlloc,
                     true}};
                m_descriptorHeaps.InitializeHeap(Resource::HeapTag::ImGui, imguiConfig);
                EngineLogger::GetInstance()->Info("[Bootstrap] ImGui TagHeap created: CBV_SRV_UAV 2048 slots");
            }
            // Single 模式下路由到 Default，无需额外初始化
            // 添加 ImGui 分区（Single = Default 堆内小分区，Multi = 独立堆内分区）
            m_descriptorHeaps.AddPartition(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, Resource::PartitionType::Texture, 0,
                                           2048, Resource::HeapTag::ImGui);
            EngineLogger::GetInstance()->Info("[Bootstrap] ImGui partition created: size=2048");
        }

        // 初始化主深度缓冲 SRV（供后处理 Pass 只读采样）
        m_deviceContext->InitDepthSRV();

        // 初始化深度模板资源池（单例）
        EngineLogger::GetInstance()->Info("[Bootstrap] Initializing DepthStencilPool...");
        DepthStencilPool::GetInstance().Initialize(m_deviceContext->GetDevice(), &m_descriptorHeaps);
        EngineLogger::GetInstance()->Info("[Bootstrap] DepthStencilPool initialized.");
        EngineLogger::GetInstance()->Info("[Bootstrap] Initializing RenderTargetPool...");
        RenderTargetPool::GetInstance().Initialize(m_deviceContext->GetDevice(), &m_descriptorHeaps);
        EngineLogger::GetInstance()->Info("[Bootstrap] RenderTargetPool initialized.");

        // ====================================================================
        // 初始化 SharedDataStore（共享数据存储层）
        // ====================================================================
        {
            EngineLogger::GetInstance()->Info("[Bootstrap] Initializing SharedDataStore...");
            auto resourceCfgPath = std::filesystem::path("Config") / "resource.json";
            Core::SharedDataStore::GetInstance().Preallocate(1024);
            if (std::filesystem::exists(resourceCfgPath)) {
                try {
                    auto resourceCfg = Boot::ConfigManager::LoadJSON<Boot::ResourceSystemConfig>(resourceCfgPath);
                    Core::SharedDataStore::GetInstance().Initialize(resourceCfg);
                } catch (const std::exception &e) {
                    EngineLogger::GetInstance()->Warn("[Bootstrap] Failed to load resource.json: {}", e.what());
                    Boot::ResourceSystemConfig defaultCfg;
                    Core::SharedDataStore::GetInstance().Initialize(defaultCfg);
                }
            } else {
                EngineLogger::GetInstance()->Warn("[Bootstrap] resource.json not found, using defaults");
                Boot::ResourceSystemConfig defaultCfg;
                Core::SharedDataStore::GetInstance().Initialize(defaultCfg);
            }
            EngineLogger::GetInstance()->Info("[Bootstrap] SharedDataStore initialized.");
        }

        // 初始化材质管理器
        EngineLogger::GetInstance()->Info("[Bootstrap] Initializing MaterialManager...");
        m_materialManager.Initialize(1024);
        EngineLogger::GetInstance()->Info("[Bootstrap] MaterialManager initialized.");

        // 初始化 AssetLoader（注入项目根目录，传入的路径已含 Content/ 前缀）
        Resource::AssetLoader::GetInstance().Initialize(m_projectConfig.Root);
        EngineLogger::GetInstance()->Info("[Bootstrap] AssetLoader initialized (root: {})", m_projectConfig.Root);

        // ====================================================================
        // 初始化 TextureManager
        // ====================================================================
        EngineLogger::GetInstance()->Info("[Bootstrap] Initializing TextureManager...");
        m_textureManager.Initialize(m_deviceContext->GetDevice(), &m_descriptorHeaps);
        EngineLogger::GetInstance()->Info("[Bootstrap] TextureManager initialized.");

        // ====================================================================
        // 初始化帧资源管理器
        // ====================================================================
        EngineLogger::GetInstance()->Info("[Bootstrap] Initializing FrameResourceManager...");

        // 加载帧资源配置
        Renderer::FrameResourceConfig frameResConfig;
        auto frameResConfigPath = std::filesystem::path("Config") / "frame_resource.json";
        if (std::filesystem::exists(frameResConfigPath)) {
            try {
                frameResConfig = Boot::ConfigManager::LoadJSON<Renderer::FrameResourceConfig>(frameResConfigPath);
            } catch (const std::exception &e) {
                EngineLogger::GetInstance()->Warn("[Bootstrap] Failed to load frame_resource.json, using defaults");
            }
        }

        m_frameResourceManager.Initialize(m_deviceContext->GetDevice(), &m_descriptorHeaps, frameResConfig);

        // 注册到 ConfigManager 托管（供后续热重载）
        Boot::ConfigManager::GetInstance().Register(
            "frame_resource", {frameResConfigPath, Boot::ConfigManager::ConfigFormat::JSON, false},
            [this](const nlohmann::json &j) {
                if (j.is_null() || !j.contains("ringBuffers"))
                    return;
                Renderer::FrameResourceConfig cfg;
                try {
                    cfg = j.get<Renderer::FrameResourceConfig>();
                } catch (...) {
                    return;
                }
                // TODO: 在线重建 RingBuffer
                (void)cfg;
            });

        EngineLogger::GetInstance()->Info("[Bootstrap] FrameResourceManager initialized.");

        // ====================================================================
        // 初始化几何体资源管理器
        // ====================================================================
        EngineLogger::GetInstance()->Info("[Bootstrap] Initializing GeometryResourceManager...");
        m_geometryResourceManager.Initialize(1024);
        EngineLogger::GetInstance()->Info("[Bootstrap] GeometryResourceManager initialized.");

        // ====================================================================
        // 初始化骨骼资源管理器
        // ====================================================================
        EngineLogger::GetInstance()->Info("[Bootstrap] Initializing SkeletonManager...");
        m_skeletonManager.Initialize(128);
        EngineLogger::GetInstance()->Info("[Bootstrap] SkeletonManager initialized.");

        m_lodSystem.SetLODConfig(LODConfig::GetDefault());
        m_lodSystem.SetCameraManager(&DX12Engine::Renderer::CameraManager::GetInstance());
        m_lodSystem.SetGeometryManager(&m_geometryResourceManager);

        InitializeDebugUI();

        // 5. MessageDispatcher 单例 (Event 层，调度系统需要)
        EngineLogger::GetInstance()->Info("[Bootstrap] Initializing MessageDispatcher...");
        Event::MessageDispatcher::Init();
        EngineLogger::GetInstance()->Info("[Bootstrap] MessageDispatcher initialized.");

        // 6. ECS Registry 初始化移至 CreateContext（由调用方接管所有权）
        // 7. FrameDriver (调度层核心，由基础设施层创建)
        // 注册表初始化后在 CreateContext 中创建 FrameDriver

        // 8. 后台任务执行器（异步资产加载等）
        EngineLogger::GetInstance()->Info("[Bootstrap] Initializing BackgroundExecutor...");
        m_backgroundExecutor = std::make_unique<Async::BackgroundExecutor>(2);
        m_backgroundExecutor->SetCommandManager(&m_deviceContext->GetCommandManager());
        EngineLogger::GetInstance()->Info("[Bootstrap] BackgroundExecutor initialized.");

        // 9. AssetManager（统一异步加载入口）
        EngineLogger::GetInstance()->Info("[Bootstrap] Initializing AssetManager...");
        Resource::AssetManager::GetInstance().Initialize(m_deviceContext.get(), m_backgroundExecutor.get(),
                                                         &m_geometryResourceManager, &m_materialManager,
                                                         &m_textureManager, &m_descriptorHeaps);
        EngineLogger::GetInstance()->Info("[Bootstrap] AssetManager initialized.");

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
    m_context->ProjectConfig = &m_projectConfig; // 项目配置
    m_context->Config = &ConfigManager::GetInstance();
    m_context->Logging = EngineLogger::GetInstance();
    m_context->Window = m_window.get();
    m_context->MainTimer = m_mainTimer.get();
    m_context->Dispatcher = Event::MessageDispatcher::GetInstance();
    m_context->BackgroundExecutor = m_backgroundExecutor.get();
    m_context->DeviceContext = m_deviceContext.get();

    // 初始化场景管理器（在 FrameDriver 之前，确保帧循环可用）
    // 先创建 World（ECS 绝对源头），再初始化 SceneManager 并绑定
    m_world.Initialize();
    m_sceneManager.Initialize(&m_world);
    m_context->SceneMgr = &m_sceneManager;

    // 初始化 FrameDriver
    InitializeFrameDriver();
    m_context->FrameDriver = m_frameDriver.get();

    m_context->CameraMgr = &DX12Engine::Renderer::CameraManager::GetInstance();

    // 初始化反射探针管理器
    m_reflectionProbeManager.Initialize(m_deviceContext->GetDevice(), &m_descriptorHeaps);

    m_context->DescriptorHeaps = &m_descriptorHeaps;
    m_context->DepthStencilPool = &DepthStencilPool::GetInstance();
    m_context->RenderTargetPool = &RenderTargetPool::GetInstance();
    m_context->FrameResourceManager = &m_frameResourceManager;

    m_context->InputMgr = &DX12Engine::Input::InputManager::Get();
    m_context->GeometryResourceManager = &m_geometryResourceManager;
    m_context->MaterialMgr = &m_materialManager;
    m_context->TextureMgr = &m_textureManager;
    m_context->SkeletonMgr = &m_skeletonManager;
    m_context->CullingSystem = &m_cullingSystem;
    m_context->LODSystem = &m_lodSystem;
    // VisibleRaycaster 初始化由 SceneManager 在接管 Registry 后处理
    // m_visibleRaycaster.Initialize(registry.get());
    m_context->VisibleRaycaster = &m_visibleRaycaster;

    // 配置 RenderScene 渲染上下文容器的指针（管理器单例 + 共享基础设施）
    {
        auto *rs = m_sceneManager.GetRenderScene();
        if (rs) {
            rs->SetLightManager(&DX12Engine::Renderer::LightManager::GetInstance());
            rs->SetReflectionProbeManager(&m_reflectionProbeManager);
            rs->SetAmbientOcclusionManager(&DX12Engine::Renderer::AmbientOcclusionManager::GetInstance());
            rs->SetDescriptorHeaps(m_context->DescriptorHeaps);
            rs->SetDeviceContext(m_context->DeviceContext);
        }
    }

    if (m_frameDriver) {
        m_frameDriver->SetGameContext(m_context.get());
    }

    auto &debugUI = DebugUI::DebugUIManager::Get();
    debugUI.SetGameContext(m_context.get());
    debugUI.AutoRegisterToFrameDriver(m_context.get());

    return m_context.get();
}

void Bootstrap::Run(const Core::ProjectConfig &config) {
    m_projectConfig = config;
    InitializeModules();
    m_isInitialized = true;
}

void Bootstrap::InitializeDebugUI() {
    EngineLogger::GetInstance()->Info("[Bootstrap] Initializing DebugUI...");

    auto &debugUI = DebugUI::DebugUIManager::Get();

    // 1. 初始化 Win32 后端
    debugUI.Initialize(m_window->GetHandle());

    // 2. 初始化 DX12 后端
    const auto &rendererConfig = m_rendererConfig;

    debugUI.InitDX12Backend(m_deviceContext->GetDevice(), m_deviceContext->GetCommandQueue(), 2,
                            rendererConfig.formats.BackBufferFormatEnum);

    // 3. 可选：配置样式和默认行为
    debugUI.ApplyDarkTheme();
    debugUI.SetShowMenuBar(true);

    // 4. 合并图标字体（由 Bootstrap 负责路径解析，DebugUIManager 不关心文件路径）
    {
        std::string iconFontPath =
            (std::filesystem::path(m_projectConfig.Root) / "Content/Fonts/iconfont.ttf").string();
        debugUI.MergeIconFont(iconFontPath);
    }

    // 5. 注册到 FrameDriver（通过 GameContext）
    // 这一步在 CreateContext 中完成，因为需要 GameContext

    EngineLogger::GetInstance()->Info("[Bootstrap] DebugUI initialized successfully");
}

} // namespace Boot
} // namespace DX12Engine
