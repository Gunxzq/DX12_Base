#include "OutlinerPanel.h"
#include "Boot/GameContext.h"
#include "ECS/Core/Components/Name.h"
#include "EditorSceneManager.h"
#include "EditorStrings.h"
#include "Scene/SceneManager.h"
#include "ThirdParty/imgui/imgui.h"
#include <cstdio>

using namespace DX12Engine;

// ========================================================================
// 初始化
// ========================================================================

void OutlinerPanel::InitializeContext(Boot::GameContext *context) { m_context = context; }

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
        auto *sceneMgr = m_editorSceneMgr->GetSceneManager();
        if (!sceneMgr) {
            ImGui::TextDisabled("(no scene manager)");
            ImGui::End();
            return;
        }

        uint32_t entityCount = 0;
        for (auto handle : m_editorSceneMgr->GetActiveEntities()) {
            auto *nameComp = sceneMgr->GetComponent<ECS::NameComponent>(handle);
            if (!nameComp)
                continue;
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
                // 选中状态在 Tab 切换/关闭时由 SceneSnapshot 统一持久化
            }
        }
        if (entityCount == 0) {
            ImGui::TextDisabled("(empty scene)");
        }
    } else {
        ImGui::TextDisabled("(no editor scene manager)");
    }

    ImGui::End();
}

// ========================================================================
// 选中状态恢复
// ========================================================================

void OutlinerPanel::RestoreSelection(uint64_t persistentId) {
    if (persistentId == 0 || !m_editorSceneMgr)
        return;

    auto *sceneMgr = m_editorSceneMgr->GetSceneManager();
    if (!sceneMgr)
        return;

    // 遍历实体，查找匹配 persistentId 的实体
    for (auto handle : m_editorSceneMgr->GetActiveEntities()) {
        auto *nameComp = sceneMgr->GetComponent<ECS::NameComponent>(handle);
        if (!nameComp)
            continue;
        if (nameComp->persistentId == persistentId) {
            m_selectedEntity = static_cast<DX12Engine::ECS::Entity>(handle);
            m_context->Logging->Info("[OutlinerPanel] Restored selection: persistentId={}, entity={}", persistentId,
                                     handle);
            return;
        }
    }
    // 未找到匹配实体（可能已被删除）
    m_selectedEntity = DX12Engine::ECS::INVALID_ENTITY;
}