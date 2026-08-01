#pragma once

#include "Boot/GameContext.h"
#include "ECS/Core/Entity.h"
#include "ThirdParty/imgui/imgui.h"
#include <DirectXMath.h>
#include <functional>

// ========================================================================
// EditorGizmoSystem — 编辑器视口 Gizmo 操纵器 + ViewCube
//
// 职责：
//   - 在视口图像上叠加绘制 ImGuizmo（Translate/Rotate）
//   - 将操作结果写回选中实体的 TransformComponent
//   - 通过回调获取操作模式（从 EditorViewportToolbar）和选中实体
//   - 在视口右上角绘制 ViewCube（XYZ 轴向指示器）
//   - ViewCube 点击后提取 ImGuizmo 修改的视图矩阵，同步回 CameraManager
//
// 使用方式：
//   1. Initialize() 时传入 GameContext
//   2. SetGetGizmoOpCallback() 注册操作模式获取回调
//   3. SetGetSelectedEntityCallback() 注册选中实体获取回调
//   4. DrawGizmo() + DrawViewCube() 注册为 EditorLayout 的视口叠加回调
// ========================================================================

class EditorGizmoSystem {
public:
    EditorGizmoSystem() = default;
    ~EditorGizmoSystem() = default;

    EditorGizmoSystem(const EditorGizmoSystem &) = delete;
    EditorGizmoSystem &operator=(const EditorGizmoSystem &) = delete;

    void Initialize(DX12Engine::Boot::GameContext *context);
    void Shutdown();

    void SetGetGizmoOpCallback(std::function<int()> cb) { m_getGizmoOp = std::move(cb); }
    void SetGetSelectedEntityCallback(std::function<DX12Engine::ECS::Entity()> cb) {
        m_getSelectedEntity = std::move(cb);
    }

    void SetVisible(bool visible) { m_visible = visible; }
    bool IsVisible() const { return m_visible; }

    void DrawGizmo(ImVec2 viewportMin, ImVec2 viewportMax);
    void DrawViewCube(ImVec2 viewportMin, ImVec2 viewportMax);

private:
    DX12Engine::Boot::GameContext *m_context = nullptr;

    std::function<int()> m_getGizmoOp;
    std::function<DX12Engine::ECS::Entity()> m_getSelectedEntity;

    bool m_visible = true;
    bool m_initialized = false;
};