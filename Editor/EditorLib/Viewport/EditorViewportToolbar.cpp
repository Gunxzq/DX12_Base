#include "EditorViewportToolbar.h"
#include "EditorViewportInputActions.h"
#include "Platform/Input/InputManager.h"
#include "Platform/Input/InputSystem.h"
#include "ThirdParty/imgui/imgui.h"
#include "ThirdParty/imguizmo/ImGuizmo.h"

using namespace DX12Engine::Input;

// ========================================================================
// 初始化/销毁
// ========================================================================

void EditorViewportToolbar::Initialize(DX12Engine::Boot::GameContext *context) {
    if (m_initialized || !context)
        return;

    m_context = context;
    m_context->Logging->Info("[EditorViewportToolbar] Initializing...");

    RegisterInputCallbacks();

    m_initialized = true;
    m_context->Logging->Info("[EditorViewportToolbar] Initialized");
}

void EditorViewportToolbar::Shutdown() {
    if (!m_initialized)
        return;
    UnregisterInputCallbacks();
    m_initialized = false;
}

// ========================================================================
// 输入回调注册（快捷键切换工具）
// ========================================================================

void EditorViewportToolbar::RegisterInputCallbacks() {
    auto *inputSys = m_context->InputMgr ? m_context->InputMgr->GetInputSystem() : nullptr;
    if (!inputSys) {
        m_context->Logging->Warn("[EditorViewportToolbar] No InputSystem available");
        return;
    }

    // ToolCursor: Q 键切换到 Cursor 工具
    m_callbackIds[0] = inputSys->BindCallback(
        ActionId_ToolCursor,
        [this](const InputActionState &) {
            SetCurrentTool(ViewportTool::Cursor);
        },
        TriggerBehavior::OnPressed);

    // ToolTranslate: W 键切换到 Translate 工具
    m_callbackIds[1] = inputSys->BindCallback(
        ActionId_ToolTranslate,
        [this](const InputActionState &) {
            SetCurrentTool(ViewportTool::Translate);
        },
        TriggerBehavior::OnPressed);

    // ToolRotate: E 键切换到 Rotate 工具
    m_callbackIds[2] = inputSys->BindCallback(
        ActionId_ToolRotate,
        [this](const InputActionState &) {
            SetCurrentTool(ViewportTool::Rotate);
        },
        TriggerBehavior::OnPressed);
}

void EditorViewportToolbar::UnregisterInputCallbacks() {
    auto *inputSys = m_context->InputMgr ? m_context->InputMgr->GetInputSystem() : nullptr;
    if (!inputSys)
        return;

    for (auto id : m_callbackIds) {
        if (id != 0)
            inputSys->UnbindCallback(id);
    }
    std::memset(m_callbackIds, 0, sizeof(m_callbackIds));
}

// ========================================================================
// 工具模式切换
// ========================================================================

void EditorViewportToolbar::SetCurrentTool(ViewportTool tool) {
    if (m_currentTool == tool)
        return;
    m_currentTool = tool;
    m_context->Logging->Info("[EditorViewportToolbar] Tool switched to {}", static_cast<int>(tool));
}

void EditorViewportToolbar::SetCursorMode(CursorMode mode) {
    m_cursorMode = mode;
}

int EditorViewportToolbar::GetCurrentGizmoOp() const {
    switch (m_currentTool) {
    case ViewportTool::Translate:
        return ImGuizmo::TRANSLATE;
    case ViewportTool::Rotate:
        return ImGuizmo::ROTATE;
    default:
        return ImGuizmo::TRANSLATE;
    }
}

// ========================================================================
// 工具栏 UI 绘制
// ========================================================================

void EditorViewportToolbar::DrawToolbar(const ImVec2 &viewportPos, const ImVec2 &viewportSize) {
    if (!m_initialized)
        return;

    ImGui::SetNextWindowPos(ImVec2(viewportPos.x + 12.0f, viewportPos.y + 12.0f));
    ImGui::SetNextWindowBgAlpha(0.6f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(6.0f, 6.0f));
    ImGui::Begin("ViewportToolbar", nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                     ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar |
                     ImGuiWindowFlags_AlwaysAutoResize);

    // ── 工具按钮组 ──
    // Q: Cursor 工具 — 点击切换 View/Select 子模式，图标随模式变化
    {
        const char *cursorIcon = (m_cursorMode == CursorMode::View)
                                     ? "\xee\xa3\x9a" // \ue8da 手形（View 模式）
                                     : "\xee\x9d\xb3"; // \ue773 光标（Select 模式）
        const char *tooltip = (m_cursorMode == CursorMode::View) ? "View (Q)" : "Select (Q)";
        bool isActive = (m_currentTool == ViewportTool::Cursor);
        DrawToolButton(cursorIcon, tooltip, ViewportTool::Cursor, isActive);
        // 点击 Cursor 按钮时，在 View/Select 之间切换
        if (isActive && ImGui::IsItemClicked()) {
            m_cursorMode = (m_cursorMode == CursorMode::View) ? CursorMode::Select : CursorMode::View;
        }
    }
    ImGui::SameLine();
    DrawToolButton("\xee\x98\x96", "Translate (W)", ViewportTool::Translate, m_currentTool == ViewportTool::Translate);
    ImGui::SameLine();
    DrawToolButton("R", "Rotate (E)", ViewportTool::Rotate, m_currentTool == ViewportTool::Rotate);

    ImGui::End();
    ImGui::PopStyleVar();
}

void EditorViewportToolbar::DrawToolButton(const char *label, const char *tooltip, ViewportTool tool, bool isActive) {
    ImVec4 bgColor = isActive ? ImVec4(0.3f, 0.5f, 0.8f, 1.0f) : ImVec4(0.2f, 0.2f, 0.2f, 0.8f);
    ImGui::PushStyleColor(ImGuiCol_Button, bgColor);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.4f, 0.6f, 0.9f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.2f, 0.4f, 0.7f, 1.0f));

    if (ImGui::Button(label, ImVec2(28, 28))) {
        SetCurrentTool(tool);
    }

    ImGui::PopStyleColor(3);

    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", tooltip);
    }
}