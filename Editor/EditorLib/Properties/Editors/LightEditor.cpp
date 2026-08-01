#include "Core/EditorStrings.h"
#include "ECS/Core/Components/Light.h"
#include "ECS/Core/Registry.h"
#include "Properties/ComponentEditorRegistry.h"
#include "ThirdParty/imgui/imgui.h"

namespace DX12Engine::ECS {

/// 注册 LightComponent 的编辑方法
void RegisterLightEditor() {
    ComponentEditorRegistry::Register<LightComponent>(
        EditorStrings::Get("component_light", "Light"), "Lighting", [](ECS::Registry *registry, ECS::Entity entity) {
            auto *lc = registry->TryGetComponent<LightComponent>(entity);
            if (!lc)
                return;

            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(4, 2));
            bool open =
                ImGui::CollapsingHeader(EditorStrings::Get("component_light", "Light"), ImGuiTreeNodeFlags_DefaultOpen);
            ImGui::PopStyleVar();
            if (!open)
                return;

            ImGui::Indent(16.0f);

            // ── 光源类型 ──
            ImGui::Text(EditorStrings::Get("component_light_type", "Type"));
            ImGui::SameLine(120);
            int type = (int)lc->type;
            ImGui::PushItemWidth(160);
            if (ImGui::Combo("##LightType", &type,
                             "Directional\0Point\0Spot\0", 3)) {
                lc->type = (float)type;
            }
            ImGui::PopItemWidth();

            // ── 颜色（RGB，0-1 范围） ──
            float color[3] = {lc->strength.x, lc->strength.y, lc->strength.z};
            ImGui::Text(EditorStrings::Get("component_light_color", "Color"));
            ImGui::SameLine(120);
            ImGui::PushItemWidth(200);
            if (ImGui::ColorEdit3("##LightColor", color,
                                  ImGuiColorEditFlags_NoInputs)) {
                lc->strength.x = color[0];
                lc->strength.y = color[1];
                lc->strength.z = color[2];
            }
            ImGui::PopItemWidth();

            // ── 强度（独立 HDR 滑条，支持 > 1.0 的大值） ──
            ImGui::Text(EditorStrings::Get("component_light_intensity", "Intensity"));
            ImGui::SameLine(120);
            ImGui::PushItemWidth(160);
            ImGui::DragFloat("##Intensity", &lc->strength.w, 0.1f, 0.0f, 100.0f, "%.2f");
            ImGui::PopItemWidth();

            // ── 点/聚光灯：Range + 衰减参数 ──
            if (type >= 1) {
                ImGui::Text(EditorStrings::Get("component_light_range", "Range"));
                ImGui::SameLine(120);
                ImGui::PushItemWidth(160);
                ImGui::SliderFloat("##Range", &lc->range, 0.1f, 100.0f, "%.1f");
                ImGui::PopItemWidth();

                ImGui::Text(EditorStrings::Get("component_light_falloff_start", "Falloff Start"));
                ImGui::SameLine(120);
                ImGui::PushItemWidth(160);
                ImGui::SliderFloat("##FalloffStart", &lc->falloffStart, 0.0f, lc->falloffEnd, "%.1f");
                ImGui::PopItemWidth();

                ImGui::Text(EditorStrings::Get("component_light_falloff_end", "Falloff End"));
                ImGui::SameLine(120);
                ImGui::PushItemWidth(160);
                ImGui::SliderFloat("##FalloffEnd", &lc->falloffEnd, lc->falloffStart, 100.0f, "%.1f");
                ImGui::PopItemWidth();
            }

            // ── 聚光灯专属：SpotPower ──
            if (type == 2) {
                ImGui::Text(EditorStrings::Get("component_light_spot_power", "Spot Power"));
                ImGui::SameLine(120);
                ImGui::PushItemWidth(160);
                ImGui::SliderFloat("##SpotPower", &lc->spotPower, 0.0f, 64.0f, "%.1f");
                ImGui::PopItemWidth();
            }

            // ── 阴影 ──
            bool castShadow = lc->castShadow > 0.0f;
            ImGui::Text(EditorStrings::Get("component_light_cast_shadow", "Cast Shadow"));
            ImGui::SameLine(120);
            if (ImGui::Checkbox("##CastShadow", &castShadow)) {
                lc->castShadow = castShadow ? 1.0f : 0.0f;
            }
            if (castShadow) {
                ImGui::Text(EditorStrings::Get("component_light_shadow_bias", "Shadow Bias"));
                ImGui::SameLine(120);
                ImGui::PushItemWidth(160);
                ImGui::SliderFloat("##ShadowBias", &lc->shadowBias, 0.0f, 0.1f, "%.4f");
                ImGui::PopItemWidth();
            }

            ImGui::Unindent(16.0f);
        });
}

} // namespace DX12Engine::ECS
