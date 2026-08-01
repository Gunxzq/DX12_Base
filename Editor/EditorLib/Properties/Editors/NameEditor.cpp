#include "Core/EditorStrings.h"
#include "ECS/Core/Components/Name.h"
#include "ECS/Core/Registry.h"
#include "Properties/ComponentEditorRegistry.h"
#include "ThirdParty/imgui/imgui.h"

namespace DX12Engine::ECS {

/// 注册 NameComponent 的编辑方法
void RegisterNameEditor() {
    ComponentEditorRegistry::Register<NameComponent>(
        EditorStrings::Get("component_name", "Name"), "General",
        [](ECS::Registry *registry, ECS::Entity entity) {
            auto *nc = registry->TryGetComponent<NameComponent>(entity);
            if (!nc)
                return;

            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(4, 2));
            bool open =
                ImGui::CollapsingHeader(EditorStrings::Get("component_name", "Name"), ImGuiTreeNodeFlags_DefaultOpen);
            ImGui::PopStyleVar();
            if (!open)
                return;

            ImGui::Indent(16.0f);

            // 实体名称编辑框
            // 使用本地 buffer 以避免直接编辑 std::string 导致迭代器失效
            static const size_t kBufSize = 256;
            char buf[kBufSize];
            strncpy_s(buf, nc->name.c_str(), kBufSize - 1);
            buf[kBufSize - 1] = '\0';

            ImGui::Text("%s", EditorStrings::Get("component_name_label", "Entity Name"));
            ImGui::SameLine(120);
            ImGui::PushItemWidth(200);
            if (ImGui::InputText("##EntityName", buf, kBufSize)) {
                nc->name = buf;
            }
            ImGui::PopItemWidth();

            // 显示 persistentId（只读）
            ImGui::Text("%s", EditorStrings::Get("component_name_id", "Persistent ID"));
            ImGui::SameLine(120);
            ImGui::TextDisabled("%llu", static_cast<unsigned long long>(nc->persistentId));

            ImGui::Unindent(16.0f);
        },
        false // Name 是核心组件，不允许移除
    );
}

} // namespace DX12Engine::ECS
