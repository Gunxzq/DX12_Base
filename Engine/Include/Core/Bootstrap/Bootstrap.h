#pragma once

#include <filesystem>
#include <memory>
#include <string>

// 先包含 Window.h（定义 Window::Desc），再包含 GameContext.h
#include "Core/Config/ConfigManager.h"
#include "Core/Context/GameContext.h"
#include "Core/Game/Game.h"
#include "Core/Window/Window.h"

namespace DX12Engine {
namespace Core {

// ========================================================================
// Bootstrap - 装配层，负责初始化基础设施和创建 GameContext
//
// 职责定位：
//   - 做什么：初始化基础设施、创建 Context、填充能力、创建 Game 实例
//   - 不做什么：不持有消息循环、不管理运行时生命周期、不进入主循环
// ========================================================================

class Game; // 前向声明

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

    /**
     * @brief 运行游戏
     * @note 调用 Game::Run()，会阻塞直到窗口关闭
     */
    void Run();

private:
    /**
     * @brief 关闭并清理
     */
    void Shutdown();

    /**
     * @brief 初始化配置管理器
     */
    void InitializeConfigManager(const std::filesystem::path &configDir);

    /**
     * @brief 初始化日志系统
     */
    void InitializeLogging();

    /**
     * @brief 创建窗口
     */
    bool CreateMainWindow();

    /**
     * @brief 创建游戏上下文
     * @return GameContext*
     * @date 2026-04-21
     */
    GameContext *CreateContext();

    /**
     * @brief 初始化模块
     * @date 2026-04-21
     */
    void InitializeModules();

    // ── 成员变量 ──

    std::unique_ptr<Window> m_window;       // 窗口
    std::unique_ptr<GameContext> m_context; // 游戏上下文
    std::unique_ptr<Game> m_game;           // 游戏实例

    // 注意：ConfigManager 和 Logger 都是单例，通过 GetInstance() 访问

    bool m_isInitialized = false;
};

} // namespace Core
} // namespace DX12Engine
