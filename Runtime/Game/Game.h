#pragma once

#include "Core/Context/GameContext.h"
#include <memory>

// ========================================================================
// Game - 游戏主逻辑层，负责运行主循环和组合游戏模块
// ========================================================================

class Game {
public:
    /**
     * @brief 构造函数
     * @param context 注入的 GameContext（由 Bootstrap 创建并填充）
     */
    explicit Game(DX12Engine::Core::GameContext *context);

    ~Game();

    // 禁止拷贝和移动
    Game(const Game &) = delete;
    Game &operator=(const Game &) = delete;
    Game(Game &&) = delete;
    Game &operator=(Game &&) = delete;

    // ── 生命周期 ──

    /**
     * @brief 初始化游戏
     * @return bool 初始化是否成功
     */
    bool Initialize();

    /**
     * @brief 运行主循环（会阻塞直到窗口关闭）
     * @return int 退出码
     */
    int Run();

    /**
     * @brief 关闭并清理游戏
     */
    void Shutdown();

    // ── 主循环组件 ──

    /**
     * @brief 每帧更新逻辑
     * @param deltaTime 帧间隔时间（秒）
     */
    void Update(float deltaTime);

    /**
     * @brief 每帧渲染
     */
    void Render();

    // ── 状态查询 ──

    bool IsRunning() const { return m_isRunning; }

private:
    // ── 内部初始化 ──

    void InitializeGameModules();
    void ShutdownGameModules();

    // ── 成员变量 ──

    DX12Engine::Core::GameContext *m_context; // 注入的上下文
    bool m_isRunning = false;                  // 运行状态
    bool m_isInitialized = false;              // 初始化状态

    // 帧时间追踪
    float m_deltaTime = 0.0f;

    // TODO: 游戏逻辑模块（后续扩展）
    // std::unique_ptr<InputManager>    m_inputManager;
    // std::unique_ptr<PlayerManager>    m_playerManager;
    // std::unique_ptr<LevelManager>     m_levelManager;
    // std::unique_ptr<AssetManager>     m_assetManager;
    // std::unique_ptr<AudioManager>     m_audioManager;
};
