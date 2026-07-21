#pragma once

#include "Core/IEditorPanel.h"
#include "ECS/Core/Entity.h"
#include <memory>

// 前向声明
namespace DX12Engine::Scene { class SceneManager; }
namespace DX12Engine::Boot { class GameContext; }

// ========================================================================
// OutlinerPanel — 编辑器场景大纲面板
//
// 职责：
//   - 显示场景中所有实体列表（层次结构）
//   - 管理实体选中状态
//   - 通过 EditorSceneManager 访问场景数据
//   - 提供 Outliner 焦点状态供输入上下文切换
// ========================================================================

class OutlinerPanel : public IEditorPanel {
public:
    OutlinerPanel() = default;
    ~OutlinerPanel() override = default;

    OutlinerPanel(const OutlinerPanel &) = delete;
    OutlinerPanel &operator=(const OutlinerPanel &) = delete;

    // ── IEditorPanel ──
    const char *GetWindowName() const override { return "Outliner###Outliner"; }
    const char *GetWindowLabelKey() const override { return "outliner"; }
    DockWindowId GetDockWindowId() const override { return DockWindowId::Outliner; }
    DockZone GetDockZone() const override { return DockZone::Left; }
    void Draw(float deltaTime) override;

    bool IsVisible() const override { return m_visible; }
    void SetVisible(bool visible) override { m_visible = visible; }

    bool Initialize() override { return true; }
    void Shutdown() override {}

    // ── 初始化（Editor 调用） ──
    void InitializeContext(DX12Engine::Boot::GameContext *context);

    // ── 设置场景管理器引用（由 Editor 传入） ──
    void SetEditorSceneManager(DX12Engine::Scene::SceneManager *mgr) { m_editorSceneMgr = mgr; }

    // ── 实体选中 ──
    DX12Engine::ECS::Entity GetSelectedEntity() const { return m_selectedEntity; }
    void ClearSelectedEntity() { m_selectedEntity = DX12Engine::ECS::INVALID_ENTITY; }

    // ── 焦点状态（供 Editor 输入上下文切换） ──
    bool IsOutlinerFocused() const { return m_outlinerFocused; }

private:
    DX12Engine::Boot::GameContext *m_context = nullptr;

    // 场景管理器引用（由 Editor 持有，不拥有所有权）
    DX12Engine::Scene::SceneManager *m_editorSceneMgr = nullptr;

    // 选中状态
    DX12Engine::ECS::Entity m_selectedEntity = DX12Engine::ECS::INVALID_ENTITY;

    // 焦点状态
    bool m_outlinerFocused = false;

    // 显隐
    bool m_visible = true;
};