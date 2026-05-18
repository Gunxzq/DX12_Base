#pragma once

#include "Core/Context/GameContext.h"
#include "Renderer/Core/PassConstants.h"
#include "Renderer/Modules/Renderer/OpaqueRenderer.h"
#include "System/ECS/Entity.h"
#include <DirectXMath.h>
#include <array>
#include <memory>
#include <wrl/client.h>

// ========================================================================
// Game - 游戏主逻辑层，负责运行主循环和组合游戏模块
// ========================================================================

class Game {
public:
    explicit Game(DX12Engine::Core::GameContext *context);
    ~Game();

    Game(const Game &) = delete;
    Game &operator=(const Game &) = delete;
    Game(Game &&) = delete;
    Game &operator=(Game &&) = delete;

    // ── 生命周期 ──
    bool Initialize();
    int Run();
    void Shutdown();

    // ── 主循环组件 ──
    void Update(float deltaTime);

    // ── 状态查询 ──
    bool IsRunning() const { return m_isRunning; }

private:
    void InitializeGameModules();
    void ShutdownGameModules();
    void CreateTestCube();

    // ── 成员变量 ──
    DX12Engine::Core::GameContext *m_context;
    std::unique_ptr<DX12Engine::Renderer::OpaqueRenderer> m_opaqueRenderer;

    DX12Engine::ECS::Entity m_cubeEntity;
    bool m_isRunning = false;
    bool m_isInitialized = false;

    // 环形缓冲区：存储每帧的 PassConstants 数据
    struct PassCBResource {
        Microsoft::WRL::ComPtr<ID3D12Resource> resource;
        void *mappedData = nullptr;
    };
    std::array<PassCBResource, 3> m_passCBResources;

    /**
     * @brief 初始化 Pass Constant Buffers
     */
    void InitializePassConstantBuffers();

    /**
     * @brief 获取当前帧 Pass Constant Buffer 的 GPU 虚拟地址
     */
    D3D12_GPU_VIRTUAL_ADDRESS GetCurrentPassCBAddress() const;
};
