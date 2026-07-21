#include "EditorLayout.h"
#include "DebugUI/DebugUIManager.h"
#include "ECS/Core/Components.h"
#include "EditorStrings.h"
#include "Platform/Windows/Window.h"
#include "Preview/PreviewManager.h"
#include "Renderer/RHI/D3D12DeviceContext.h"
#include "Renderer/Scene/CameraManager.h"
#include "Renderer/Scene/GridManager.h"
#include "ThirdParty/imgui/imgui.h"
#include "ThirdParty/imgui/imgui_internal.h"
#include <DirectXMath.h>
#include <algorithm>
#include <cstring>

using namespace DX12Engine;
using namespace DebugUI;

EditorLayout::EditorLayout(Boot::GameContext *context) : m_context(context) {}

EditorLayout::~EditorLayout() { Shutdown(); }

bool EditorLayout::Initialize() {
    if (!m_context)
        return false;

    m_context->Logging->Info("[EditorLayout] Initializing editor layout...");

    // 关闭 DebugUIManager 的菜单栏（避免干扰编辑器布局）
    DebugUI::DebugUIManager::Get().SetShowMenuBar(false);

    // 视口尺寸默认跟随窗口
    m_viewportWidth = m_context->Window->GetWidth();
    m_viewportHeight = m_context->Window->GetHeight();

    // 初始化多语言字符串
    if (m_context->ProjectConfig) {
        std::string configDir = m_context->ProjectConfig->Root + "/" + m_context->ProjectConfig->ConfigRoot;
        EditorStrings::Initialize(configDir);
    }

    m_initialized = true;
    m_context->Logging->Info("[EditorLayout] Editor layout initialized");
    return true;
}

void EditorLayout::Shutdown() {
    if (!m_initialized)
        return;

    // 通知所有已注册面板清理
    for (auto *panel : m_panels) {
        panel->Shutdown();
    }
    m_panels.clear();

    m_initialized = false;
    m_dockLayoutInitialized = false;
}

void EditorLayout::Register() {
    DebugUIManager::Get().RegisterRootDrawCallback([this](float dt, uint32_t) { Draw(dt); });
}

void EditorLayout::Draw(float deltaTime) {
    // 关闭 ImGui 内部创建的 Debug##Default fallback 窗口，防止影响鼠标事件
    if (ImGui::FindWindowByName("Debug##Default")) {
        // 移到屏幕外并标记为不可见，避免拦截鼠标事件
        ImGui::SetWindowPos("Debug##Default", ImVec2(-9999, -9999), ImGuiCond_Always);
        ImGui::SetWindowSize("Debug##Default", ImVec2(0, 0), ImGuiCond_Always);
    }

    DrawMenuBar();
    DrawDockSpace();
    DrawStatusBar();

    // 尚未独立为 Panel 的内置面板（后续逐步迁移）
    if (m_showViewport)
        DrawViewport();
    if (m_showProperties)
        DrawProperties();

    // 遍历已注册的 IEditorPanel 面板
    for (auto *panel : m_panels) {
        if (panel->IsVisible()) {
            panel->Draw(deltaTime);
        }
    }
}

// ── 面板注册 ──

/// 构建翻译后的窗口名：EditorStrings::Get(labelKey, fallback) + DockWindowIdToStr(id)
static std::string MakeTranslatedWindowName(IEditorPanel *panel) {
    std::string label = EditorStrings::Get(panel->GetWindowLabelKey(), "");
    if (label.empty()) {
        // 语言包未加载或 key 不存在，取 GetWindowName() 中 ### 前的部分
        const char *winName = panel->GetWindowName();
        const char *hashId = std::strchr(winName, '#');
        label = hashId ? std::string(winName, hashId - winName) : winName;
    }
    return label + DockWindowIdToStr(panel->GetDockWindowId());
}

void EditorLayout::RegisterPanel(IEditorPanel *panel) {
    if (!panel)
        return;

    // 避免重复注册
    for (auto *p : m_panels) {
        if (p == panel)
            return;
    }

    m_panels.push_back(panel);

    // 如果 dock 布局已初始化，立即绑定窗口到 dock 节点
    if (m_dockLayoutInitialized) {
        ImGuiID dockId = GetDockId(panel->GetDockZone());
        if (dockId != 0) {
            std::string translatedName = MakeTranslatedWindowName(panel);
            ImGui::DockBuilderDockWindow(translatedName.c_str(), dockId);
        }
    }
}

void EditorLayout::UnregisterPanel(const char *windowName) {
    auto it = std::remove_if(m_panels.begin(), m_panels.end(), [windowName](IEditorPanel *panel) {
        return strcmp(panel->GetWindowName(), windowName) == 0;
    });
    if (it != m_panels.end()) {
        m_panels.erase(it, m_panels.end());
    }
}

IEditorPanel *EditorLayout::FindPanel(const char *windowName) {
    for (auto *panel : m_panels) {
        if (strcmp(panel->GetWindowName(), windowName) == 0)
            return panel;
    }
    return nullptr;
}

// ── Dock 布局 ──

ImGuiID EditorLayout::GetDockId(DockZone zone) const {
    switch (zone) {
    case DockZone::Left:
        return m_dockLeft;
    case DockZone::LeftBottom:
        return m_dockLeftBottom;
    case DockZone::Center:
        return m_dockCenter;
    case DockZone::CenterBottom:
        return m_dockCenterBottom;
    case DockZone::Right:
        return m_dockRight;
    default:
        return 0;
    }
}

void EditorLayout::InitializeDockLayout(ImGuiID dockspaceId) {
    ImGui::DockBuilderRemoveNode(dockspaceId);
    ImGui::DockBuilderAddNode(dockspaceId, ImGuiDockNodeFlags_DockSpace);
    ImGui::DockBuilderSetNodeSize(dockspaceId, ImGui::GetMainViewport()->Size);

    ImGuiID dockMain = dockspaceId;

    // 水平拆分主区域：左、中、右
    m_dockLeft = ImGui::DockBuilderSplitNode(dockMain, ImGuiDir_Left, 0.20f, nullptr, &dockMain);
    m_dockRight = ImGui::DockBuilderSplitNode(dockMain, ImGuiDir_Right, 0.22f, nullptr, &dockMain);
    m_dockCenter = dockMain;

    // 左列垂直拆分：上(Outliner) 下(Content Browser / 资源管理器)
    m_dockLeftBottom = ImGui::DockBuilderSplitNode(m_dockLeft, ImGuiDir_Down, 0.40f, nullptr, &m_dockLeft);

    // 中间区域垂直拆分：上(Viewport) 下(Console)
    m_dockCenterBottom = ImGui::DockBuilderSplitNode(m_dockCenter, ImGuiDir_Down, 0.30f, nullptr, &m_dockCenter);

    // ── 绑定内置面板（尚未独立为 Panel） ──
    ImGui::DockBuilderDockWindow(
        (std::string(EditorStrings::Get("viewport", "Viewport")) + DockWindowIdToStr(DockWindowId::Viewport)).c_str(),
        m_dockCenter);
    ImGui::DockBuilderDockWindow(
        (std::string(EditorStrings::Get("properties", "Properties")) + DockWindowIdToStr(DockWindowId::Properties))
            .c_str(),
        m_dockRight);

    // ── 绑定已注册的 IEditorPanel 面板（使用翻译后的窗口名） ──
    for (auto *panel : m_panels) {
        ImGuiID dockId = GetDockId(panel->GetDockZone());
        if (dockId != 0) {
            std::string translatedName = MakeTranslatedWindowName(panel);
            ImGui::DockBuilderDockWindow(translatedName.c_str(), dockId);
        }
    }

    ImGui::DockBuilderFinish(dockspaceId);
}

// ── 菜单栏 ──

void EditorLayout::DrawMenuBar() {
    if (ImGui::BeginMainMenuBar()) {
        if (ImGui::BeginMenu(EditorStrings::Get("menu_file", "File"))) {
            if (ImGui::MenuItem(EditorStrings::Get("menu_file_new", "New Scene"), "Ctrl+N")) {
            }
            if (ImGui::MenuItem(EditorStrings::Get("menu_file_open", "Open Scene"), "Ctrl+O")) {
            }
            if (ImGui::MenuItem(EditorStrings::Get("menu_file_save", "Save Scene"), "Ctrl+S")) {
            }
            if (ImGui::MenuItem(EditorStrings::Get("menu_file_save_as", "Save As..."), "Ctrl+Shift+S")) {
            }
            ImGui::Separator();
            if (ImGui::MenuItem(EditorStrings::Get("menu_file_exit", "Exit"), "Alt+F4")) {
                m_context->Window->SetFullscreen(false);
                PostMessage(m_context->Window->GetHandle(), WM_CLOSE, 0, 0);
            }
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu(EditorStrings::Get("menu_edit", "Edit"))) {
            if (ImGui::MenuItem(EditorStrings::Get("menu_edit_undo", "Undo"), "Ctrl+Z")) {
            }
            if (ImGui::MenuItem(EditorStrings::Get("menu_edit_redo", "Redo"), "Ctrl+Y")) {
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Duplicate", "Ctrl+D")) {
            }
            if (ImGui::MenuItem("Delete", "Del")) {
            }
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu(EditorStrings::Get("menu_view", "View"))) {
            // 内置面板的显隐
            ImGui::MenuItem(EditorStrings::Get("viewport", "Viewport"), nullptr, &m_showViewport);
            ImGui::MenuItem(EditorStrings::Get("properties", "Properties"), nullptr, &m_showProperties);
            ImGui::MenuItem(EditorStrings::Get("status_bar", "Status Bar"), nullptr, &m_showStatusBar);

            // 已注册的 IEditorPanel 面板的显隐
            ImGui::Separator();
            for (auto *panel : m_panels) {
                bool visible = panel->IsVisible();
                if (ImGui::MenuItem(EditorStrings::Get(panel->GetWindowLabelKey(), panel->GetWindowName()), nullptr,
                                    &visible)) {
                    panel->SetVisible(visible);
                }
            }
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu(EditorStrings::Get("menu_gizmo", "Gizmo"))) {
            if (ImGui::MenuItem(EditorStrings::Get("menu_gizmo_translate", "Translate"), "W", m_gizmoOperation == 0))
                m_gizmoOperation = 0;
            if (ImGui::MenuItem(EditorStrings::Get("menu_gizmo_rotate", "Rotate"), "E", m_gizmoOperation == 1))
                m_gizmoOperation = 1;
            if (ImGui::MenuItem(EditorStrings::Get("menu_gizmo_scale", "Scale"), "R", m_gizmoOperation == 2))
                m_gizmoOperation = 2;
            ImGui::Separator();
            ImGui::MenuItem(EditorStrings::Get("menu_view_show_grid", "Show Grid"), nullptr, &m_showGrid);
            ImGui::MenuItem(EditorStrings::Get("menu_view_show_gizmo", "Show Gizmo"), nullptr, &m_showGizmo);
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu(EditorStrings::Get("help", "Help"))) {
            if (ImGui::MenuItem("ImGui Demo")) {
                DebugUI::DebugUIManager::Get().SetShowDemoWindow(true);
            }
            ImGui::EndMenu();
        }

        // ── 右侧：语言切换 ──
        const auto &locales = EditorStrings::GetAvailableLocales();
        if (!locales.empty()) {
            const std::string &current = EditorStrings::GetLocale();
            int currentIdx = 0;
            std::vector<const char *> localeNames;
            for (const auto &l : locales) {
                if (l == current)
                    currentIdx = (int)localeNames.size();
                localeNames.push_back(l.c_str());
            }
            float menuBarWidth = ImGui::GetWindowWidth();
            float langWidth = 80.0f;
            ImGui::SetCursorPosX(menuBarWidth - langWidth - 8);
            ImGui::SetNextItemWidth(langWidth);
            if (ImGui::Combo("##Lang", &currentIdx, localeNames.data(), (int)localeNames.size())) {
                EditorStrings::SetLocale(locales[currentIdx]);
            }
        }

        ImGui::EndMainMenuBar();
    }
}

// ── Dockspace ──

void EditorLayout::DrawDockSpace() {
    const ImGuiViewport *viewport = ImGui::GetMainViewport();
    float statusBarHeight = m_showStatusBar ? 24.0f : 0.0f;
    ImGui::SetNextWindowPos(viewport->WorkPos);
    ImGui::SetNextWindowSize(ImVec2(viewport->WorkSize.x, viewport->WorkSize.y - statusBarHeight));
    ImGui::SetNextWindowViewport(viewport->ID);

    ImGuiWindowFlags dockFlags = ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoTitleBar |
                                 ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                                 ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus;

    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);

    ImGui::Begin("DockSpace", nullptr, dockFlags);
    ImGui::PopStyleVar(2);

    ImGuiID dockspaceId = ImGui::GetID("EditorDockSpace");
    ImGui::DockSpace(dockspaceId);

    // 首次运行时初始化拆分布局
    if (!m_dockLayoutInitialized) {
        InitializeDockLayout(dockspaceId);
        m_dockLayoutInitialized = true;
    }

    ImGui::End();
}

// ── 视口 ──

void EditorLayout::DrawViewport() {
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::Begin(
        (std::string(EditorStrings::Get("viewport", "Viewport")) + DockWindowIdToStr(DockWindowId::Viewport)).c_str(),
        &m_showViewport, ImGuiWindowFlags_NoScrollbar);

    m_viewportHovered = ImGui::IsWindowHovered();

    ImVec2 contentSize = ImGui::GetContentRegionAvail();
    m_viewportWidth = static_cast<uint32_t>(contentSize.x);
    m_viewportHeight = static_cast<uint32_t>(contentSize.y);

    if (m_viewportSRV.ptr != 0) {
        ImVec2 contentTopLeft = ImGui::GetCursorScreenPos();
        ImGui::Image((ImTextureID)m_viewportSRV.ptr, contentSize);

        // 网格比例尺滑条（覆盖在视口内容区左上角）
        ImGui::SetCursorScreenPos(ImVec2(contentTopLeft.x + 8, contentTopLeft.y + 8));
        ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.1f, 0.1f, 0.15f, 0.7f));
        ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, ImVec4(0.15f, 0.15f, 0.2f, 0.8f));
        ImGui::PushStyleColor(ImGuiCol_SliderGrab, ImVec4(0.4f, 0.6f, 1.0f, 1.0f));
        float spacing = Renderer::GridManager::GetInstance().GetMinorSpacing();
        ImGui::SetNextItemWidth(120.0f);
        if (ImGui::SliderFloat("##GridSpacing", &spacing, 1.0f, 500.0f, "Grid: %.0f")) {
            Renderer::GridManager::GetInstance().SetMinorSpacing(spacing);
            Renderer::GridManager::GetInstance().SetMajorSpacing(spacing * 10.0f);
        }
        ImGui::PopStyleColor(3);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Grid spacing in world units");
    } else {
        // 占位：深灰色背景
        ImDrawList *drawList = ImGui::GetWindowDrawList();
        ImVec2 posMin = ImGui::GetCursorScreenPos();
        ImVec2 posMax = ImVec2(posMin.x + contentSize.x, posMin.y + contentSize.y);
        drawList->AddRectFilled(posMin, posMax, IM_COL32(40, 40, 50, 255));
        ImGui::Dummy(contentSize);

        const char *hint = "Viewport - No Render Target";
        ImVec2 textSize = ImGui::CalcTextSize(hint);
        ImVec2 textPos =
            ImVec2(posMin.x + (contentSize.x - textSize.x) * 0.5f, posMin.y + (contentSize.y - textSize.y) * 0.5f);
        drawList->AddText(textPos, IM_COL32(120, 120, 140, 255), hint);
    }

    ImGui::End();
    ImGui::PopStyleVar();
}

// ── 属性面板（含预览 + 组件属性） ──

void EditorLayout::DrawProperties() {
    ImGui::Begin(
        (std::string(EditorStrings::Get("properties", "Properties")) + DockWindowIdToStr(DockWindowId::Properties))
            .c_str(),
        &m_showProperties);

    // ── 顶部：资产预览（如有） ──
    if (m_previewManager && m_previewId != 0) {
        ImVec2 contentSize = ImGui::GetContentRegionAvail();
        float previewHeight = std::min(contentSize.y * 0.45f, 300.0f);
        ImVec2 previewSize(contentSize.x, previewHeight);

        D3D12_GPU_DESCRIPTOR_HANDLE srv = m_previewManager->GetOutputSRV(m_previewId);
        if (srv.ptr != 0 && previewSize.x > 0 && previewSize.y > 0) {
            auto *ctx = m_previewManager->GetContext(m_previewId);

            // 约束宽高比
            ImVec2 imageSize = previewSize;
            if (ctx && ctx->width > 0 && ctx->height > 0) {
                float rtAspect = (float)ctx->width / (float)ctx->height;
                float windowAspect = previewSize.x / previewSize.y;
                if (windowAspect > rtAspect) {
                    imageSize.x = previewSize.y * rtAspect;
                } else {
                    imageSize.y = previewSize.x / rtAspect;
                }
            }
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (previewSize.x - imageSize.x) * 0.5f);
            ImGui::Image((ImTextureID)srv.ptr, imageSize);
            bool hovered = ImGui::IsItemHovered();

            // Orbit 相机控制
            ImGuiIO &io = ImGui::GetIO();
            if (hovered && io.MouseDown[0]) {
                m_previewCamTheta -= io.MouseDelta.x * 0.005f;
                m_previewCamPhi = std::clamp(m_previewCamPhi - io.MouseDelta.y * 0.005f, 0.1f, 1.4f);
            }
            if (hovered) {
                m_previewCamRadius = std::clamp(m_previewCamRadius - io.MouseWheel, 1.0f, 20.0f);
            }

            // 更新相机位置
            if (ctx) {
                ctx->position.x = ctx->target.x + m_previewCamRadius * cosf(m_previewCamTheta) * cosf(m_previewCamPhi);
                ctx->position.y = ctx->target.y + m_previewCamRadius * sinf(m_previewCamPhi);
                ctx->position.z = ctx->target.z + m_previewCamRadius * sinf(m_previewCamTheta) * cosf(m_previewCamPhi);
            }

            // 光照参数
            if (ctx && ctx->renderMode == PreviewRenderMode::PBR) {
                ImGui::Separator();
                ImGui::Text("Light");
                ImGui::SameLine();
                ImGui::PushItemWidth(80);
                ImGui::SliderFloat("Strength", &m_previewLightStrength, 0.0f, 10.0f, "%.1f");
                ImGui::SameLine();
                ImGui::SliderFloat("X", &m_previewLightAngleX, 0.0f, 360.0f, "%.0f");
                ImGui::SameLine();
                ImGui::SliderFloat("Y", &m_previewLightAngleY, 0.0f, 90.0f, "%.0f");
                ImGui::PopItemWidth();

                float radX = m_previewLightAngleX * 3.14159265f / 180.0f;
                float radY = m_previewLightAngleY * 3.14159265f / 180.0f;
                ctx->lightDirection.x = cosf(radY) * sinf(radX);
                ctx->lightDirection.y = -sinf(radY);
                ctx->lightDirection.z = cosf(radY) * cosf(radX);
                ctx->lightStrength = m_previewLightStrength;
                ctx->needsRender = true;
            }
        }

        ImGui::Separator();
    }

    // ── 下方：组件属性 ──
    ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.8f, 1.0f), "No entity selected");
    ImGui::Separator();
    ImGui::Text("Transform");
    ImGui::Indent(16.0f);
    float pos[3] = {0, 0, 0};
    float rot[3] = {0, 0, 0};
    float scale[3] = {1, 1, 1};
    ImGui::DragFloat3("Position", pos, 0.1f);
    ImGui::DragFloat3("Rotation", rot, 0.1f);
    ImGui::DragFloat3(EditorStrings::Get("menu_gizmo_scale", "Scale"), scale, 0.1f);
    ImGui::Unindent(16.0f);
    ImGui::Separator();
    ImGui::Text("Mesh");
    ImGui::Indent(16.0f);
    ImGui::TextDisabled("(none)");
    ImGui::Unindent(16.0f);

    ImGui::End();
}

// ── 状态栏 ──

void EditorLayout::DrawStatusBar() {
    if (!m_showStatusBar)
        return;

    ImGuiViewport *viewport = ImGui::GetMainViewport();
    float statusBarHeight = 24.0f;

    ImGui::SetNextWindowPos(ImVec2(viewport->Pos.x, viewport->Pos.y + viewport->Size.y - statusBarHeight));
    ImGui::SetNextWindowSize(ImVec2(viewport->Size.x, statusBarHeight));
    ImGui::SetNextWindowViewport(viewport->ID);

    ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                             ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoSavedSettings |
                             ImGuiWindowFlags_NoFocusOnAppearing;

    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::Begin("StatusBar", nullptr, flags);
    ImGui::PopStyleVar();

    ImGui::Text("DX12 Editor");

    if (m_context && m_context->MainTimer) {
        float deltaTime = m_context->MainTimer->GetDeltaTime();
        float fps = 1.0f / std::max(deltaTime, 0.0001f);
        ImGui::SameLine(ImGui::GetWindowWidth() - 160);
        ImGui::Text("FPS: %.1f  (%.1f ms)", fps, deltaTime * 1000.0f);
    }

    ImGui::End();
}