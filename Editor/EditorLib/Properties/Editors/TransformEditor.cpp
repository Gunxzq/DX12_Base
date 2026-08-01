#include "Core/EditorStrings.h"
#include "ECS/Core/Components/Transform.h"
#include "ECS/Core/Registry.h"
#include "Properties/ComponentEditorRegistry.h"
#include "ThirdParty/imgui/imgui.h"
#include <DirectXMath.h>

namespace DX12Engine::ECS {

/// 注册 TransformComponent 的编辑方法
void RegisterTransformEditor() {
    ComponentEditorRegistry::Register<TransformComponent>(
        EditorStrings::Get("component_transform", "Transform"), "Transform",
        [](ECS::Registry *registry, ECS::Entity entity) {
            // Transform 是核心组件，不允许移除
            auto *tc = registry->TryGetComponent<TransformComponent>(entity);
            if (!tc)
                return;

            // 可折叠分组
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(4, 2));
            bool open = ImGui::CollapsingHeader(EditorStrings::Get("component_transform", "Transform"),
                                                ImGuiTreeNodeFlags_DefaultOpen);
            ImGui::PopStyleVar();

            if (!open)
                return;

            ImGui::Indent(16.0f);
            ImGui::PushItemWidth(120);

            // 位置
            ImGui::DragFloat3(EditorStrings::Get("component_transform_position", "Position"), &tc->position.x, 0.1f);

            // 旋转：四元数 → 欧拉角（度）用于显示，编辑时转回四元数
            // 这是 UI 边界的唯一转换，非热路径，不影响渲染一致性
            {
                // 从旋转矩阵提取 XYZ (RollPitchYaw) 欧拉角
                DirectX::XMVECTOR quat = DirectX::XMLoadFloat4(&tc->rotation);
                DirectX::XMMATRIX rotMat = DirectX::XMMatrixRotationQuaternion(quat);
                float pitch, yaw, roll;
                // R = Rz(roll) * Ry(yaw) * Rx(pitch)
                float sy = std::sqrt(rotMat.r[0].m128_f32[0] * rotMat.r[0].m128_f32[0] +
                                     rotMat.r[1].m128_f32[0] * rotMat.r[1].m128_f32[0]);
                if (sy > 1e-6f) {
                    pitch = std::atan2(-rotMat.r[2].m128_f32[1], rotMat.r[2].m128_f32[2]);
                    yaw = std::atan2(rotMat.r[2].m128_f32[0], sy);
                    roll = std::atan2(-rotMat.r[1].m128_f32[0], rotMat.r[0].m128_f32[0]);
                } else {
                    pitch = std::atan2(-rotMat.r[2].m128_f32[1], rotMat.r[2].m128_f32[2]);
                    yaw = std::atan2(-rotMat.r[2].m128_f32[0], sy);
                    roll = 0.0f;
                }
                float rotDeg[3] = {DirectX::XMConvertToDegrees(pitch), DirectX::XMConvertToDegrees(yaw),
                                   DirectX::XMConvertToDegrees(roll)};
                if (ImGui::DragFloat3(EditorStrings::Get("component_transform_rotation", "Rotation"), rotDeg, 0.5f,
                                      -360.0f, 360.0f)) {
                    DirectX::XMVECTOR newQuat = DirectX::XMQuaternionRotationRollPitchYaw(
                        DirectX::XMConvertToRadians(rotDeg[0]), DirectX::XMConvertToRadians(rotDeg[1]),
                        DirectX::XMConvertToRadians(rotDeg[2]));
                    DirectX::XMStoreFloat4(&tc->rotation, newQuat);
                }
            }

            // 缩放
            ImGui::DragFloat3(EditorStrings::Get("component_transform_scale", "Scale"), &tc->scale.x, 0.05f, 0.01f,
                              1000.0f);

            ImGui::PopItemWidth();
            ImGui::Unindent(16.0f);
        },
        false // Transform 是核心组件，不允许移除
    );
}

} // namespace DX12Engine::ECS
