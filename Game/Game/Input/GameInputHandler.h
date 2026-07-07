#pragma once

#include "ECS/Core/Entity.h"
#include "Math/MathTypes.h"
#include "Scheduler/Task.h"
#include <DirectXMath.h>

namespace DX12Engine {
namespace Boot {
class GameContext;
}
namespace ECS {
class Registry;
}
namespace Platform {
class Window;
}
namespace Input {
class InputManager;
}
namespace Renderer {
class CameraManager;
class VisibleRaycaster;
struct RaycastHit;
struct PredictedCameraData;
} // namespace Renderer
} // namespace DX12Engine

/**
 * @brief 游戏输入处理器
 *
 * 职责：
 * 1. 处理相机控制输入（WASD 移动、鼠标旋转）- 龙书风格第一人称相机
 * 2. 管理鼠标捕获状态
 * 3. 注册自己的输入处理系统到 ECS 调度器
 * 4. 拾取系统集成（射线检测 + 拖拽移动）
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

    /**
     * @brief 设置射线检测器引用
     */
    void SetVisibleRaycaster(DX12Engine::Renderer::VisibleRaycaster *raycaster) { m_visibleRaycaster = raycaster; }

    // =========================================================================
    // 拖拽逻辑
    // EarlyUpdate: 纯数学计算意图（不碰 ECS）
    // FrameSync:   ApplyDragToECS() 安全地更新 ECS 组件
    // =========================================================================

    enum class DragIntent { None, Start, Update, End };

    /// FrameSync 回调中调用：安全地将拖拽意图应用到 ECS 组件
    void ApplyDragToECS(DX12Engine::ECS::Registry &registry);

    /// 清理拖拽状态
    void EndDragCleanup();

private:
    /**
     * @brief 注册输入处理系统到 ECS 调度器
     */
    void RegisterInputSystem();

    /**
     * @brief 注册拾取+拖拽系统到 EarlyUpdate 阶段
     */
    void RegisterPickingSystems();

    /**
     * @brief 处理相机输入（移动和旋转）
     * @param deltaTime 帧间隔时间
     */
    void HandleCameraInput(float deltaTime);

    /**
     * @brief 处理鼠标捕获状态切换
     */
    void HandleCursorCapture();

    // =========================================================================
    // 龙书风格相机操作
    // =========================================================================

    /// 绕 Right 轴旋转（上下看）
    void Pitch(float angle);

    /// 绕世界 Y 轴旋转（左右看）
    void RotateY(float angle);

    /// 沿 Right 轴平移（左右移动）
    void Strafe(float d);

    /// 沿 Forward 轴平移（前后移动）
    void Walk(float d);

private:
    DX12Engine::Boot::GameContext *m_context = nullptr;

    // 跳过第一帧的鼠标输入（避免切换捕获时的视角跳变）
    bool m_skipLookInputThisFrame = false;

    // 相机移动参数
    float m_moveSpeed = 10.0f;
    float m_sprintMultiplier = 2.5f;

    // 垂直升降速度
    float m_verticalSpeed = 6.0f;

    // 射线检测工具（引擎层，由 Bootstrap 创建）
    DX12Engine::Renderer::VisibleRaycaster *m_visibleRaycaster = nullptr;

    // ── 拖拽状态 ──
    bool m_isDragging = false;
    DX12Engine::ECS::Entity m_dragEntity = DX12Engine::ECS::INVALID_ENTITY;
    DirectX::XMFLOAT3 m_dragOffset = {0.0f, 0.0f, 0.0f}; // entityPos - hitPoint

    float m_dragDepth = 0.0f; // 拾取深度（沿相机前向到命中点的距离）

    // ── 拖拽意图（EarlyUpdate 计算，FrameSync 应用）──
    DragIntent m_pendingDragIntent = DragIntent::None;

    DX12Engine::ECS::Entity m_pendingHitEntity = DX12Engine::ECS::INVALID_ENTITY;

    DirectX::XMFLOAT3 m_pendingHitPoint = {};
    DirectX::XMFLOAT3 m_pendingDragPosition = {};
};
