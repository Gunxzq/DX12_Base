#include "OutlinerPanel.h"
#include "Boot/GameContext.h"
#include "EditorStrings.h"
#include "Scene/SceneManager.h"
#include "ThirdParty/imgui/imgui.h"
#include <cstdio>

using namespace DX12Engine;

// ========================================================================
// 初始化
// ========================================================================

void OutlinerPanel::InitializeContext(Boot::GameContext *context) {
    m_context = context;
}

// ========================================================================
// 每帧绘制
// ========================================================================

void OutlinerPanel::Draw(float deltaTime) {
    if (!m_visible)
        return;

    ImGui::Begin(GetWindowName(), &m_visible);

    // 记录焦点状态（供 Editor 输入上下文切换）
    m_outlinerFocused = ImGui::IsWindowFocused();

    ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.8f, 1.0f), "Scene Hierarchy");
    ImGui::Separator();

    if (m_editorSceneMgr) {
        uint32_t entityCount = 0;
        for (auto handle : m_editorSceneMgr->GetAllEntities()) {
            auto* nameComp = m_editorSceneMgr->GetComponent<ECS::NameComponent>(handle);
            if (!nameComp) continue;
            entityCount++;

            char label[128];
            snprintf(label, sizeof(label), "%s##outliner_%llu", nameComp->name.c_str(),
                     static_cast<unsigned long long>(nameComp->persistentId));

            ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;
            auto entity = static_cast<DX12Engine::ECS::Entity>(handle);
            if (m_selectedEntity == entity)
                flags |= ImGuiTreeNodeFlags_Selected;

            ImGui::TreeNodeEx(label, flags);
            if (ImGui::IsItemClicked()) {
                m_selectedEntity = entity;
            }
        }
        if (entityCount == 0) {
            ImGui::TextDisabled("(empty scene)");
        }
    } else {
        ImGui::TextDisabled("(no scene manager)");
    }

    ImGui::End();
}