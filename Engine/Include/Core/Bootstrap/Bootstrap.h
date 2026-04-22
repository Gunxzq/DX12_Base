#pragma once

#include <filesystem>
#include <memory>
#include <string>

#include "System/Window/Window.h"

namespace DX12Engine {
namespace Core {

// ========================================================================
// 前向声明 (移到命名空间内部)
// ========================================================================
class GameContext;

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

    /**
     * @brief 初始化模块
     */
    void Run();

    /**
     * @brief 创建游戏上下文
     * @return GameContext*
     * @date 2026-04-21
     */
    GameContext *CreateContext();

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
     * @brief 初始化模块
     * @date 2026-04-21
     */
    void InitializeModules();

    // ── 成员变量 ──

    std::unique_ptr<Window> m_window;       // 窗口
    std::unique_ptr<GameContext> m_context; // 游戏上下文

    // 注意：ConfigManager 和 Logger 都是单例，通过 GetInstance() 访问

    bool m_isInitialized = false;
};

} // namespace Core
} // namespace DX12Engine
