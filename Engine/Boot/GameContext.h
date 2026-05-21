#pragma once

#include "GameTimer.h"
#include "Logger/Logger.h"

namespace DX12Engine {
namespace Input {
class InputSystem;
}
namespace Renderer {
class D3D12DeviceContext;
class CommandManager;
class CameraManager;
} // namespace Renderer

namespace Event {
class MessageDispatcher;
}

namespace ECS {
class Registry;
}

namespace Scheduler {
class FrameDriver;
}

namespace Logger {
class Logger;
}

namespace Platform {
class Window;
}

namespace Boot {

// ========================================================================
// 前向声明
// ========================================================================

class ConfigManager;
class GameTimer;

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

    Platform::Window *Window = nullptr;             // 窗口管理
    ConfigManager *Config = nullptr;                // 配置管理
    Logger::Logger *Logging = nullptr;              // 日志系统
    GameTimer *MainTimer = nullptr;                 // 主计时器
    Event::MessageDispatcher *Dispatcher = nullptr; // 消息分发器（单例）

    // ── 调度与数据层指针 ──

    Scheduler::FrameDriver *FrameDriver = nullptr; // 帧驱动器
    ECS::Registry *Registry = nullptr;             // ECS 注册表

    // ── 渲染子系统指针 ──

    Renderer::D3D12DeviceContext *DeviceContext = nullptr; // D3D12 设备上下文
    Renderer::CommandManager *CommandManager = nullptr;    // 命令管理器（由 DeviceContext 管理）
    Renderer::CameraManager *CameraMgr = nullptr;          // 相机管理器

    Input::InputSystem *InputSys = nullptr; // 输入系统

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

} // namespace Boot
} // namespace DX12Engine
