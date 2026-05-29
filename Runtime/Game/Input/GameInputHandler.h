#pragma once

#include "ECS/Core/Entity.h"
#include "Scheduler/Task.h"
#include <DirectXMath.h>

namespace DX12Engine {
namespace Boot {
class GameContext;
}
namespace Platform {
class Window;
}
namespace Input {
class InputManager;
}
namespace Renderer {
class CameraManager;
}
} // namespace DX12Engine

/**
 * @brief 游戏输入处理器
 *
 * 职责：
 * 1. 处理相机控制输入（WASD 移动、鼠标旋转）
 * 2. 管理鼠标捕获状态
 * 3. 注册自己的输入处理系统到 ECS 调度器
 */
class GameInputHandler {
public:
    GameInputHandler();
    ~GameInputHandler();

    // 禁止拷贝
    GameInputHandler(const GameInputHandler &) = delete;
    GameInputHandler &operator=(const GameInputHandler &) = delete;

    // 允许移动
    GameInputHandler(GameInputHandler &&) noexcept = default;
    GameInputHandler &operator=(GameInputHandler &&) noexcept = default;

    /**
     * @brief 初始化输入处理器
     * @param context 游戏上下文
     */
    void Initialize(DX12Engine::Boot::GameContext *context);

    /**
     * @brief 重置相机位置到默认值
     */
    void ResetCamera();

private:
    /**
     * @brief 注册输入处理系统到 ECS 调度器
     */
    void RegisterInputSystem();

    /**
     * @brief 处理相机输入（移动和旋转）
     * @param deltaTime 帧间隔时间
     */
    void HandleCameraInput(float deltaTime);

    /**
     * @brief 处理鼠标捕获状态切换
     */
    void HandleCursorCapture();

private:
    DX12Engine::Boot::GameContext *m_context = nullptr;

    // 跳过第一帧的鼠标输入（避免切换捕获时的视角跳变）
    bool m_skipLookInputThisFrame = false;

    // 自由相机参数
    float m_mouseSensitivity = 0.002f;
    float m_moveSpeed = 8.0f;
    float m_sprintMultiplier = 2.5f;

    // 垂直升降速度 (Q/E 键)
    float m_verticalSpeed = 6.0f;
};
