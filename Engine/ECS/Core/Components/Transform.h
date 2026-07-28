#pragma once
#include <DirectXMath.h>

namespace DX12Engine::ECS {

// 变换组件（位置、旋转、缩放）
// rotation 使用四元数 [x, y, z, w]，与 .scene.json 格式一致
struct TransformComponent {
    DirectX::XMFLOAT3 position = {0.0f, 0.0f, 0.0f};
    DirectX::XMFLOAT4 rotation = {0.0f, 0.0f, 0.0f, 1.0f}; // 四元数
    DirectX::XMFLOAT3 scale = {1.0f, 1.0f, 1.0f};

    DirectX::XMMATRIX GetMatrix() const {
        DirectX::XMMATRIX world = DirectX::XMMatrixScaling(scale.x, scale.y, scale.z);
        world *= DirectX::XMMatrixRotationQuaternion(DirectX::XMLoadFloat4(&rotation));
        world *= DirectX::XMMatrixTranslation(position.x, position.y, position.z);
        return world;
    }
};

// 点位置组件
struct PositionComponent {
    DirectX::XMFLOAT3 position;
};

} // namespace DX12Engine::ECS
