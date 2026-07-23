#pragma once

#include "Boot/GameContext.h"
#include "Platform/Input/InputSystem.h"
#include "ThirdParty/imgui/imgui.h"
#include <cstdint>
#include <memory>

// ========================================================================
// 工具模式定义
// ========================================================================

enum class ViewportTool : uint8_t {
    Cursor,     // 光标（含 View / Select 子模式）
    Translate,  // Gizmo 平移
    Rotate,     // Gizmo 旋转
    Count
};

enum class CursorMode : uint8_t {
    View,   // 平移/旋转相机
    Select  // 射线检测选中实体
};

// ========================================================================
// EditorViewportToolbar — 编辑器视口工具栏
//
// 职责：
//   - 管理工具模式状态（ViewportTool + CursorMode）
//   - 在视口左上角叠加 ImGui 工具栏
//   - 快捷键切换工具模式
// ========================================================================

class EditorViewportToolbar {
public:
    EditorViewportToolbar() = default;
    ~EditorViewportToolbar() = default;

    EditorViewportToolbar(const EditorViewportToolbar &) = delete;
    EditorViewportToolbar &operator=(const EditorViewportToolbar &) = delete;

    void Initialize(DX12Engine::Boot::GameContext *context);
    void Shutdown();

    /// 注册输入回调（快捷键切换工具）
    void RegisterInputCallbacks();

    /// 注销输入回调
    void UnregisterInputCallbacks();

    /// 绘制工具栏 UI（在视口图像上叠加）
    void DrawToolbar(const ImVec2 &viewportPos, const ImVec2 &viewportSize);

    /// 获取/设置当前工具
    ViewportTool GetCurrentTool() const { return m_currentTool; }
    void SetCurrentTool(ViewportTool tool);

    /// 获取/设置当前 Cursor 子模式
    CursorMode GetCursorMode() const { return m_cursorMode; }
    void SetCursorMode(CursorMode mode);

    /// 获取当前 ImGuizmo 操作类型
    int GetCurrentGizmoOp() const;

private:
    void DrawToolButton(const char *label, const char *tooltip, ViewportTool tool, bool isActive);

    DX12Engine::Boot::GameContext *m_context = nullptr;

    // 回调 ID
    DX12Engine::Input::ActionCallbackId m_callbackIds[3] = {};

    // 工具模式状态
    ViewportTool m_currentTool = ViewportTool::Cursor;
    CursorMode m_cursorMode = CursorMode::View;

    bool m_initialized = false;
};