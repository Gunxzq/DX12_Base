#include "ECS/Core/Components/Transform.h"
#include "ECS/Core/Registry.h"
#include "Properties/ComponentEditorRegistry.h"
#include "ThirdParty/imgui/imgui.h"

namespace DX12Engine::ECS {

/// 注册 TransformComponent 的编辑方法
void RegisterTransformEditor() {
    ComponentEditorRegistry::Register<TransformComponent>(
        "Transform", "Transform",
        [](ECS::Registry *registry, ECS::Entity entity) {
            auto *tc = registry->TryGetComponent<TransformComponent>(entity);
            if (!tc)
                return;

            // 可折叠分组
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(4, 2));
            bool open = ImGui::CollapsingHeader("Transform", ImGuiTreeNodeFlags_DefaultOpen);
            ImGui::PopStyleVar();

            if (!open)
                return;

            ImGui::Indent(16.0f);
            ImGui::PushItemWidth(120);

            // 位置
            ImGui::DragFloat3("Position", &tc->position.x, 0.1f);
            // 旋转（欧拉角，度）
            float rotDeg[3] = {
                DirectX::XMConvertToDegrees(tc->rotation.x),
                DirectX::XMConvertToDegrees(tc->rotation.y),
                DirectX::XMConvertToDegrees(tc->rotation.z)
            };
            if (ImGui::DragFloat3("Rotation", rotDeg, 0.5f, -360.0f, 360.0f)) {
                tc->rotation.x = DirectX::XMConvertToRadians(rotDeg[0]);
                tc->rotation.y = DirectX::XMConvertToRadians(rotDeg[1]);
                tc->rotation.z = DirectX::XMConvertToRadians(rotDeg[2]);
            }
            // 缩放
            ImGui::DragFloat3("Scale", &tc->scale.x, 0.05f, 0.01f, 1000.0f);

            ImGui::PopItemWidth();
            ImGui::Unindent(16.0f);
        }
    );
}

} // namespace DX12Engine::ECS