#pragma once

#include "Core/Context/GameContext.h"
#include <memory>

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

    // ── 成员变量 ──
    DX12Engine::Core::GameContext *m_context;
    bool m_isRunning = false;
    bool m_isInitialized = false;
    float m_deltaTime = 0.0f;
};
