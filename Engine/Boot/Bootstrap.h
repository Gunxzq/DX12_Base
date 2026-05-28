#pragma once

#include "Common/Common.h"
#include "DebugUI/DebugUIManager.h"
#include "ECS/Core/Registry.h"
#include "GameTimer.h"
#include "Platform/Windows/Window.h"
#include "Renderer/FrameResources/FrameResourceManager.h"
#include "Renderer/RHI/D3D12DeviceContext.h"
#include "Resource/Core/DescriptorHeapCollection.h"
#include "Resource/Manager/GeometryResourceManager.h"
#include "Resource/Manager/MaterialManager.h"
#include "Resource/Texture/TextureManager.h"

namespace DX12Engine {

namespace Scheduler {
class FrameDriver;
}
namespace ECS {
class Registry;
}

namespace Platform {
class Window;
}

namespace Boot {
// ========================================================================
// 前向声明 (移到命名空间内部)
// ========================================================================
class GameContext;
class GameTimer;

// ========================================================================
// Bootstrap - 装配层，负责初始化基础设施和创建 GameContext
// ========================================================================

class Bootstrap {
public:
    Bootstrap() = default;
    ~Bootstrap();

    // 禁止拷贝和移动
    Bootstrap(const Bootstrap &) = delete;
    Bootstrap &operator=(const Bootstrap &) = delete;
    Bootstrap(Bootstrap &&) = delete;
    Bootstrap &operator=(Bootstrap &&) = delete;

    // ── 生命周期 ──
    void Run();

    GameContext *CreateContext();
    ECS::Registry &GetRegistry();

    void Shutdown();

private:
    void InitializeConfigManager(const std::filesystem::path &configDir);
    void InitializeLogging();
    bool CreateMainWindow();
    bool InitializeD3DDeviceContext();
    void InitializeRegistry();
    void InitializeFrameDriver();
    void InitializeDebugUI();

    void InitializeModules();

    // ── 成员变量 ──
    std::unique_ptr<Platform::Window> m_window;                    // 窗口
    std::unique_ptr<GameContext> m_context;                        // 游戏上下文
    std::unique_ptr<GameTimer> m_mainTimer;                        // 主计时器
    std::unique_ptr<Renderer::D3D12DeviceContext> m_deviceContext; // D3D12 设备上下文
    std::unique_ptr<ECS::Registry> m_registry;                     // ECS Registry
    Scheduler::FrameDriver *m_frameDriver = nullptr;               // FrameDriver (由基础设施层创建)
    Resource::DescriptorHeapCollection m_descriptorHeaps;
    Renderer::FrameResourceManager m_frameResourceManager;
    Resource::GeometryResourceManager m_geometryResourceManager;

    Resource::MaterialManager m_materialManager;
    Resource::TextureManager m_textureManager;

    // 注意：ConfigManager 和 Logger 都是单例，通过 GetInstance() 访问

    bool m_isInitialized = false;
};

} // namespace Boot
} // namespace DX12Engine
