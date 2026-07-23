#include "ECS/Core/Components/Light.h"
#include "ECS/Core/Registry.h"
#include "Properties/ComponentEditorRegistry.h"
#include "ThirdParty/imgui/imgui.h"

namespace DX12Engine::ECS {

/// 注册 LightComponent 的编辑方法
void RegisterLightEditor() {
    ComponentEditorRegistry::Register<LightComponent>(
        "Light", "Lighting",
        [](ECS::Registry *registry, ECS::Entity entity) {
            auto *lc = registry->TryGetComponent<LightComponent>(entity);
            if (!lc)
                return;

            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(4, 2));
            bool open = ImGui::CollapsingHeader("Light", ImGuiTreeNodeFlags_DefaultOpen);
            ImGui::PopStyleVar();
            if (!open)
                return;

            ImGui::Indent(16.0f);
            ImGui::PushItemWidth(120);

            // 光源类型
            const char *lightTypes[] = {"Directional", "Point", "Spot"};
            int type = (int)lc->type;
            if (ImGui::Combo("Type", &type, lightTypes, IM_ARRAYSIZE(lightTypes))) {
                lc->type = (float)type;
            }

            // 颜色（RGB 强度合并）
            float color[4] = {lc->strength.x, lc->strength.y, lc->strength.z, lc->strength.w};
            if (ImGui::ColorEdit3("Color", color)) {
                lc->strength.x = color[0];
                lc->strength.y = color[1];
                lc->strength.z = color[2];
            }

            // 强度（单独控制亮度）
            ImGui::SliderFloat("Intensity", &lc->strength.w, 0.0f, 10.0f, "%.2f");

            // 点/聚光灯参数
            if (type >= 1) {
                ImGui::SliderFloat("Range", &lc->range, 0.1f, 100.0f, "%.1f");
            }

            // 聚光灯参数
            if (type == 2) {
                ImGui::SliderFloat("Falloff Start", &lc->falloffStart, 0.0f, lc->falloffEnd, "%.1f");
                ImGui::SliderFloat("Falloff End", &lc->falloffEnd, lc->falloffStart, 100.0f, "%.1f");
                ImGui::SliderFloat("Spot Power", &lc->spotPower, 0.0f, 64.0f, "%.1f");
            }

            // 阴影参数
            bool castShadow = lc->castShadow > 0.0f;
            if (ImGui::Checkbox("Cast Shadow", &castShadow)) {
                lc->castShadow = castShadow ? 1.0f : 0.0f;
            }
            if (castShadow) {
                ImGui::SliderFloat("Shadow Bias", &lc->shadowBias, 0.0f, 0.1f, "%.4f");
            }

            ImGui::PopItemWidth();
            ImGui::Unindent(16.0f);
        }
    );
}

} // namespace DX12Engine::ECS