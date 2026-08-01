#include "Core/EditorStrings.h"
#include "ECS/Core/Components/Camera.h"
#include "ECS/Core/Registry.h"
#include "Properties/ComponentEditorRegistry.h"
#include "ThirdParty/imgui/imgui.h"

namespace DX12Engine::ECS {

/// 注册 CameraComponent 的编辑方法
void RegisterCameraEditor() {
    ComponentEditorRegistry::Register<CameraComponent>(
        EditorStrings::Get("component_camera", "Camera"), "Camera", [](ECS::Registry *registry, ECS::Entity entity) {
            auto *cc = registry->TryGetComponent<CameraComponent>(entity);
            if (!cc)
                return;

            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(4, 2));
            bool open = ImGui::CollapsingHeader(EditorStrings::Get("component_camera", "Camera"),
                                                ImGuiTreeNodeFlags_DefaultOpen);
            ImGui::PopStyleVar();
            if (!open)
                return;

            ImGui::Indent(16.0f);

            // ── 投影类型（透视/正交） ──
            ImGui::Text(EditorStrings::Get("component_camera_projection", "Projection"));
            ImGui::SameLine(120);
            ImGui::PushItemWidth(160);
            int projType = (cc->projection == ProjectionType::Orthographic) ? 1 : 0;
            if (ImGui::Combo("##Projection", &projType, "Perspective\0Orthographic\0", 2)) {
                cc->projection = (projType == 1) ? ProjectionType::Orthographic : ProjectionType::Perspective;
            }
            ImGui::PopItemWidth();

            if (cc->projection == ProjectionType::Perspective) {
                ImGui::Text(EditorStrings::Get("component_camera_fov", "Field of View"));
                ImGui::SameLine(120);
                ImGui::PushItemWidth(160);
                ImGui::SliderFloat("##Fov", &cc->fov, 1.0f, 179.0f, "%.1f°");
                ImGui::PopItemWidth();
            } else {
                ImGui::Text(EditorStrings::Get("component_camera_ortho_size", "Ortho Size"));
                ImGui::SameLine(120);
                ImGui::PushItemWidth(160);
                ImGui::SliderFloat("##OrthoSize", &cc->orthoSize, 0.1f, 100.0f, "%.1f");
                ImGui::PopItemWidth();
            }

            // ── 近/远裁剪面 ──
            ImGui::Text(EditorStrings::Get("component_camera_near", "Near Plane"));
            ImGui::SameLine(120);
            ImGui::PushItemWidth(160);
            ImGui::SliderFloat("##Near", &cc->nearPlane, 0.01f, cc->farPlane - 0.01f, "%.2f");
            ImGui::PopItemWidth();

            ImGui::Text(EditorStrings::Get("component_camera_far", "Far Plane"));
            ImGui::SameLine(120);
            ImGui::PushItemWidth(160);
            // far 上限 1000：行业通常（Blender/Unity 默认远裁剪面），对齐 Gizmo 视锥体显示截断（kDisplayFarLimit=1000），
            // 避免 5700 这类极端值导致视锥体远裁剪面线段不可见
            ImGui::SliderFloat("##Far", &cc->farPlane, cc->nearPlane + 0.01f, 1000.0f, "%.1f");
            ImGui::PopItemWidth();

            // ── 主相机 ──
            ImGui::Text(EditorStrings::Get("component_camera_is_main", "Main Camera"));
            ImGui::SameLine(120);
            ImGui::Checkbox("##MainCamera", &cc->isMain);
            ImGui::Unindent(16.0f);
        });
}

} // namespace DX12Engine::ECS
