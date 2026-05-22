#pragma once

#include "Boot/GameContext.h"
#include "Input/GameInputHandler.h"
#include "Renderer/Pipeline/OpaqueRenderer.h"
#include "Scene/GameWorld.h"
#include <memory>

// ========================================================================
// Game - 游戏主逻辑层，负责运行主循环和组合游戏模块
// ========================================================================

class Game {
public:
    explicit Game(DX12Engine::Boot::GameContext *context);
    ~Game();

    Game(const Game &) = delete;
    Game &operator=(const Game &) = delete;
    Game(Game &&) = delete;
    Game &operator=(Game &&) = delete;

    bool Initialize();
    int Run();
    void Shutdown();

    bool IsRunning() const { return m_isRunning; }

private:
    void RegisterEngineSystems(); // WindowResizeSystem, FullscreenSystem 等引擎级系统

private:
    DX12Engine::Boot::GameContext *m_context;
    std::unique_ptr<DX12Engine::Renderer::OpaqueRenderer> m_opaqueRenderer;

    GameWorld m_world;
    GameInputHandler m_inputHandler;

    bool m_isRunning = false;
    bool m_isInitialized = false;
};