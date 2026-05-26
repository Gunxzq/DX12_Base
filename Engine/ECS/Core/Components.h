#pragma once

#include "Math/BoundingVolume.h"
#include "Resource/Geometry/TriangleMesh.h"
#include "Resource/Struct/GeometryHandle.h"
#include "Resource/Struct/LODMeshHandle.h"
#include <DirectXMath.h>
#include <d3d12.h>

namespace DX12Engine {
namespace Math {
using BoundingVolumeVariant = Math::BoundingVolumeVariant;
}

namespace Resource {

struct LODMeshHandle;
struct GeometryHandle;
} // namespace Resource
namespace ECS {

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

// 网格组件 （GeometryHandle）
// ECS/Core/Components.h
struct MeshComponent {
    Resource::LODMeshHandle lodMeshHandle;
    Math::BoundingVolumeVariant localBounds;

    bool IsValid() const { return lodMeshHandle.IsValid(); }
};
} // namespace ECS
} // namespace DX12Engine