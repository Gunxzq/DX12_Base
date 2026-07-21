#pragma once

#include "Boot/GameContext.h"
#include "Core/IEditorPanel.h"
#include "ECS/Core/Entity.h"
#include "Preview/PreviewContext.h"
#include "ThirdParty/imgui/imgui.h"
#include <DirectXMath.h>
#include <memory>
#include <unordered_map>
#include <vector>

class PreviewManager;

// ========================================================================
// EditorLayout - 编辑器 ImGui 布局管理器
//
// 职责限定：
//   1. 注册 RootDrawCallback（通过 DebugUIManager）
//   2. 定义 Dock 布局拆分（左/中/右 → 再垂直拆分）
//   3. 遍历已注册的 IEditorPanel，调用其 Draw(float)
//   4. 管理菜单栏和状态栏（全局布局元素）
//   5. 管理视口信息（SRV/尺寸/悬停，因涉及 ImGui::Image 交互）
//
// 禁止持有具体面板的成员变量或转发方法。
// 面板由 Editor 直接持有并通过 RegisterPanel 注册。
// ========================================================================

class EditorLayout {
public:
    explicit EditorLayout(DX12Engine::Boot::GameContext *context);
    ~EditorLayout();

    EditorLayout(const EditorLayout &) = delete;
    EditorLayout &operator=(const EditorLayout &) = delete;

    bool Initialize();
    void Shutdown();

    /// 每帧绘制编辑器布局（在 ImGui::NewFrame 之后、ImGui::Render 之前调用）
    void Draw(float deltaTime);

    /// 注册编辑器布局（通过 DebugUIManager 面板系统）
    void Register();

    bool IsInitialized() const { return m_initialized; }

    // ── 面板注册 ──

    /// 注册面板（在 EditorLayout::Initialize 之后调用）
    /// @param panel 面板指针（生命周期由调用方管理）
    void RegisterPanel(IEditorPanel *panel);

    /// 注销面板
    void UnregisterPanel(const char *windowName);

    /// 获取已注册的面板列表
    const std::vector<IEditorPanel *> &GetPanels() const { return m_panels; }

    /// 按窗口名查找面板
    IEditorPanel *FindPanel(const char *windowName);

    // ── 视口信息（供外部设置） ──

    void SetViewportSize(uint32_t w, uint32_t h) {
        m_viewportWidth = w;
        m_viewportHeight = h;
    }
    uint32_t GetViewportWidth() const { return m_viewportWidth; }
    uint32_t GetViewportHeight() const { return m_viewportHeight; }

    /// 视口悬停状态（供 Editor 输入上下文切换）
    bool IsViewportHovered() const { return m_viewportHovered; }

    /// 视口 RT 的 GPU SRV 句柄（供 ImGui::Image 使用）
    D3D12_GPU_DESCRIPTOR_HANDLE GetViewportSRV() const { return m_viewportSRV; }
    void SetViewportSRV(D3D12_GPU_DESCRIPTOR_HANDLE srv) { m_viewportSRV = srv; }

    // ── 预览（Properties 面板内嵌，暂未独立为 Panel） ──
    void SetPreviewManager(PreviewManager *mgr) { m_previewManager = mgr; }
    PreviewId GetPreviewId() const { return m_previewId; }
    void SetPreviewId(PreviewId id) { m_previewId = id; }
    void ShowPreviewPanel() { m_showPreview = true; }

private:
    void DrawMenuBar();
    void DrawDockSpace();
    void DrawViewport();
    void DrawProperties();
    void DrawStatusBar();

    void InitializeDockLayout(ImGuiID dockspaceId);

    /// DockZone → ImGuiID 映射（在 InitializeDockLayout 中填充）
    ImGuiID GetDockId(DockZone zone) const;

private:
    DX12Engine::Boot::GameContext *m_context;

    // ── 注册的面板 ──
    std::vector<IEditorPanel *> m_panels;

    // ── Dockspace ──
    ImGuiID m_dockspaceId = 0;
    bool m_dockLayoutInitialized = false;

    // ── Dock 节点 ID 映射 ──
    ImGuiID m_dockLeft = 0;
    ImGuiID m_dockLeftBottom = 0;
    ImGuiID m_dockCenter = 0;
    ImGuiID m_dockCenterBottom = 0;
    ImGuiID m_dockRight = 0;

    // ── 视口 ──
    D3D12_GPU_DESCRIPTOR_HANDLE m_viewportSRV = {};
    uint32_t m_viewportWidth = 0;
    uint32_t m_viewportHeight = 0;
    bool m_viewportHovered = false;

    // ── 布局状态 ──
    bool m_initialized = false;

    // 面板可见性（由 Layout 管理的面板，尚未独立为 Panel 类）
    bool m_showViewport = true;
    bool m_showProperties = true;
    bool m_showStatusBar = true;

    // 编辑器状态
    bool m_showGrid = true;
    bool m_showGizmo = true;
    int m_gizmoOperation = 0; // 0=Translate, 1=Rotate, 2=Scale

    // ── 预览（Properties 面板内嵌，暂未独立为 Panel） ──
    PreviewManager *m_previewManager = nullptr;
    PreviewId m_previewId = 0;
    bool m_showPreview = false;
    float m_previewCamTheta = 0.0f;
    float m_previewCamPhi = 0.5f;
    float m_previewCamRadius = 5.0f;
    float m_previewLightStrength = 3.0f;
    float m_previewLightAngleX = 45.0f;
    float m_previewLightAngleY = 30.0f;
};