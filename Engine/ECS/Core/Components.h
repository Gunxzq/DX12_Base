#pragma once

#include "Common/Common.h"

#include "Math/BoundingVolume.h"
#include "Resource/Geometry/TriangleMesh.h"
#include "Resource/Struct/GeometryHandle.h"
#include "Resource/Struct/LODMeshHandle.h"
#include "Resource/Struct/MaterialHandle.h"
#include "Resource/Struct/TextureHandle.h"

namespace DX12Engine {
namespace Math {
using BoundingVolumeVariant = Math::BoundingVolumeVariant;
}

namespace Resource {

struct LODMeshHandle;
struct GeometryHandle;
struct MaterialHandle;
struct TextureHandle;
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
struct MeshComponent {
    Resource::LODMeshHandle lodMeshHandle;
    Resource::MaterialHandle materialHandle;
    Resource::TextureHandle textureHandle;

    bool receivesShadow = true;

    Math::BoundingVolumeVariant localBounds;
    bool IsValid() const { return lodMeshHandle.IsValid(); }
};

// 透明网格组件（用于水、玻璃等）
struct TransparentMeshComponent {
    Resource::LODMeshHandle lodMeshHandle;
    Resource::MaterialHandle materialHandle;
    Resource::TextureHandle textureHandle;

    Math::BoundingVolumeVariant localBounds;
    bool IsValid() const { return lodMeshHandle.IsValid(); }
};

struct TerrainComponent {
    Resource::GeometryHandle geometryHandle; // PatchMesh 句柄
    Resource::TextureHandle heightMapHandle; // 高度图纹理
    Resource::TextureHandle albedoHandle;    // 漫反射纹理（可选）
    Resource::TextureHandle normalHandle;    // 法线贴图（可选）

    float heightScale = 20.0f;
    float heightOffset = 0.0f;
    float tessellationFactor = 32.0f;      // 近距离最大细分因子 (1~64)
    float tessellationDistanceMin = 10.0f; // 在此距离内使用最大细分
    float tessellationDistanceMax = 60.0f; // 超出此距离不再细分

    uint32_t materialIndex = 0;
    Math::BoundingVolumeVariant localBounds;

    // 运行时数据（由 TerrainManager 管理）
    uint32_t constantBufferOffset = 0;
    bool needsUpload = true;

    bool IsValid() const { return geometryHandle.IsValid() && heightMapHandle.IsValid(); }
};

// 公告牌组件

enum class BillboardMode : uint8_t {
    AxisY,    // 绕 Y 轴旋转（树木、灯柱）
    Full,     // 完全面向相机（粒子、闪光）
    Spherical // 球面朝向（云、远处物体）
};

struct BillboardComponent {
    Resource::TextureHandle textureHandle;
    Resource::MaterialHandle materialHandle;

    float width = 2.0f;
    float height = 4.0f;
    BillboardMode mode = BillboardMode::AxisY;

    float minDistance = 10.0f;    // 最小显示距离（近裁剪）
    float maxDistance = 500.0f;   // 最大显示距离（远裁剪）
    float switchDistance = 50.0f; // 切换到实例化 3D 模型的距离

    uint32_t textureArrayIndex = 0;

    bool IsValid() const { return textureHandle.IsValid(); }
};

// 标记组件：附加到需要持久化缓存的实体上
struct StaticComponent {
    D3D12_GPU_VIRTUAL_ADDRESS persistentCBAddress = 0;       // 持久化常量缓冲区地址
    D3D12_GPU_VIRTUAL_ADDRESS persistentInstanceAddress = 0; // 持久化实例数据地址
    uint32_t batchInstanceIndex = UINT32_MAX;

    bool worldDirty = true;

    DirectX::XMFLOAT4X4 cachedWorld;             // 缓存的 World 矩阵
    DirectX::XMFLOAT4X4 cachedWorldInvTranspose; // 缓存的 WorldInvTranspose
    float cachedDistanceToCamera = 0.0f;         // 缓存的到相机距离（LOD 用）
};
} // namespace ECS
} // namespace DX12Engine