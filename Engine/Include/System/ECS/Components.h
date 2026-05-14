// System/ECS/Components.h 或类似位置
#pragma once
#include "System/Resource/ResourceHandle.h"
#include <DirectXMath.h>
#include <d3d12.h>

namespace DX12Engine::ECS {

// 变换组件（位置、旋转、缩放）
struct TransformComponent {
    DirectX::XMFLOAT3 position = {0.0f, 0.0f, 0.0f};
    DirectX::XMFLOAT3 rotation = {0.0f, 0.0f, 0.0f}; // 欧拉角或四元数
    DirectX::XMFLOAT3 scale = {1.0f, 1.0f, 1.0f};

    DirectX::XMMATRIX GetMatrix() const {
        DirectX::XMMATRIX world = DirectX::XMMatrixScaling(scale.x, scale.y, scale.z);
        world *= DirectX::XMMatrixRotationRollPitchYaw(rotation.x, rotation.y, rotation.z);
        world *= DirectX::XMMatrixTranslation(position.x, position.y, position.z);
        return world;
    }
};

// 网格组件（GPU 资源句柄）
struct MeshComponent {
    DX12Engine::System::Resource::ResourceHandle vertexBuffer; // 通过 ResourceManager 管理
    DX12Engine::System::Resource::ResourceHandle indexBuffer;
    uint32_t vertexCount = 0;
    uint32_t indexCount = 0;

    // 调试/临时：直接存储 D3D12 视图（简化版）
    D3D12_VERTEX_BUFFER_VIEW vertexBufferView = {};
    D3D12_INDEX_BUFFER_VIEW indexBufferView = {};
};

} // namespace DX12Engine::ECS