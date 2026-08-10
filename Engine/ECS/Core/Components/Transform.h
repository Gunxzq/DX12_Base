#pragma once
#include <DirectXMath.h>
#include <algorithm> // std::max（GetEffectiveCullDistance 缩放联动）
#include <cmath>     // std::abs

namespace DX12Engine::ECS {

// 变换组件（位置、旋转、缩放）
// rotation 使用四元数 [x, y, z, w]，与 .scene.json 格式一致
struct TransformComponent {
    DirectX::XMFLOAT3 position = {0.0f, 0.0f, 0.0f};
    DirectX::XMFLOAT4 rotation = {0.0f, 0.0f, 0.0f, 1.0f}; // 四元数
    DirectX::XMFLOAT3 scale = {1.0f, 1.0f, 1.0f};
    // 剔除距离（世界空间基准，0 = 不限制）：离得足够远根本看不清的内容强制剔除（MPD @CullFar 球体剔除）。
    // 承载于变换组件而非 MeshComponent：缩放会改变实际可视距离（有效距离 = cullDistance × maxScale）
    float cullDistance = 0.0f;

    DirectX::XMMATRIX GetMatrix() const {
        DirectX::XMMATRIX world = DirectX::XMMatrixScaling(scale.x, scale.y, scale.z);
        world *= DirectX::XMMatrixRotationQuaternion(DirectX::XMLoadFloat4(&rotation));
        world *= DirectX::XMMatrixTranslation(position.x, position.y, position.z);
        return world;
    }

    /// 缩放联动后的有效剔除距离（maxScale 放大可视距离）
    float GetEffectiveCullDistance() const {
        if (cullDistance <= 0.0f)
            return 0.0f;
        float maxScale = (std::max)({(std::abs)(scale.x), (std::abs)(scale.y), (std::abs)(scale.z)});
        return cullDistance * (maxScale > 0.0f ? maxScale : 1.0f);
    }
};

// 点位置组件
struct PositionComponent {
    DirectX::XMFLOAT3 position;
};

} // namespace DX12Engine::ECS
