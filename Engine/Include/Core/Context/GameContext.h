#pragma once

#include <memory>

namespace DX12Engine {
namespace Core {

// ========================================================================
// 前向声明
// ========================================================================

class Window;
class ConfigManager;
class Logger;

// ========================================================================
// GameContext - 依赖注入容器
// 持有所有基础设施子系统的指针，通过依赖注入提供给各系统使用
// ========================================================================

class GameContext {
public:
    GameContext() = default;
    ~GameContext() = default;

    // 禁止拷贝和移动（Context 应该通过指针传递）
    GameContext(const GameContext &) = delete;
    GameContext &operator=(const GameContext &) = delete;
    GameContext(GameContext &&) = delete;
    GameContext &operator=(GameContext &&) = delete;

    // ── 基础设施子系统指针 ──

    Window *Window = nullptr;        // 窗口管理
    ConfigManager *Config = nullptr; // 配置管理
    Logger *Logging = nullptr;       // 日志系统

    // ── 便捷访问方法 ──

    /**
     * @brief 检查 Context 是否有效（所有必需字段都已填充）
     */
    bool IsValid() const;

    /**
     * @brief 获取验证失败的第一个原因（用于调试）
     */
    const char *GetInvalidReason() const;

    mutable const char *m_invalidReason = nullptr;
};

} // namespace Core
} // namespace DX12Engine
