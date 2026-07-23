#pragma once

#include "Boot/GameContext.h"
#include "Platform/Input/InputSystem.h"
#include "Viewport/EditorViewportToolbar.h"
#include <DirectXMath.h>

// ========================================================================
// EditorCameraSystem — 编辑器视口相机控制
//
// 职责：
//   - 通过输入回调响应 Move/Look/OrbitCamera/Zoom/Pan/FocusSelection 动作
//   - 直接写 CameraMgr（非 ECS 组件，允许在 Immediate 回调中执行）
//   - 每帧更新 PassConstants（View/Proj 矩阵写入 FrameResourceManager）
// ========================================================================

class EditorCameraSystem {
public:
    EditorCameraSystem() = default;
    ~EditorCameraSystem() = default;

    EditorCameraSystem(const EditorCameraSystem &) = delete;
    EditorCameraSystem &operator=(const EditorCameraSystem &) = delete;

    void Initialize(DX12Engine::Boot::GameContext *context);
    void Shutdown();

    /// 注册输入回调（由 Initialize 调用）
    void RegisterInputCallbacks();

    /// 注销输入回调（由 Shutdown 调用）
    void UnregisterInputCallbacks();

    /// 设置视口尺寸（影响相机宽高比）
    void SetViewportSize(uint32_t width, uint32_t height) {
        m_viewportWidth = width;
        m_viewportHeight = height;
    }

    /// 设置视口悬停状态（由 Editor 每帧更新）
    void SetViewportHovered(bool hovered) { m_viewportHovered = hovered; }

    /// 聚焦到指定实体
    void FocusOnEntity(DX12Engine::ECS::Entity entity, float defaultDistance = 10.0f);

    /// 设置场景管理器引用（用于 FocusOnEntity）
    void SetEditorSceneManager(DX12Engine::Scene::SceneManager *mgr) { m_editorSceneMgr = mgr; }

    /// 设置工具模式查询回调（用于判断当前是 View/Select 模式）
    void SetGetCurrentToolCallback(std::function<ViewportTool()> cb) { m_getCurrentTool = std::move(cb); }
    void SetGetCursorModeCallback(std::function<CursorMode()> cb) { m_getCursorMode = std::move(cb); }

    /// 每帧更新 PassConstants（由 Immediate 回调调用）
    void UpdatePassConstants();

private:
    // 龙书风格相机操作
    void Pitch(float angle);
    void RotateY(float angle);
    void Strafe(float d);
    void Walk(float d);

    DX12Engine::Boot::GameContext *m_context = nullptr;
    DX12Engine::Scene::SceneManager *m_editorSceneMgr = nullptr;
    DX12Engine::Input::InputSystem *m_inputSystem = nullptr;

    // 工具模式查询回调（从 EditorViewportToolbar 获取当前模式）
    std::function<ViewportTool()> m_getCurrentTool;
    std::function<CursorMode()> m_getCursorMode;

    // 回调 ID（用于注销）
    DX12Engine::Input::ActionCallbackId m_callbackIds[9] = {};

    // 相机移动参数
    float m_moveSpeed = 50.0f;
    float m_sprintMultiplier = 3.0f;
    float m_verticalSpeed = 30.0f;
    float m_panSpeed = 50.0f;

    uint32_t m_viewportWidth = 1280;
    uint32_t m_viewportHeight = 720;
    bool m_viewportHovered = false;
    bool m_initialized = false;
};