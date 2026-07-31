#pragma once
#include "Resource/Geometry/TriangleMesh.h"
#include "Resource/Struct/GeometryHandle.h"
#include "Resource/Struct/LODMeshHandle.h"
#include "Resource/Struct/TextureHandle.h"
#include "Renderer/Material/MaterialHandle.h"

namespace DX12Engine {
namespace Resource {

struct MaterialHandle;

} // namespace Resource

namespace Math {
using BoundingVolumeVariant = Math::BoundingVolumeVariant;
} // namespace Math

namespace ECS {

// 网格组件（所有可渲染几何体统一使用）
struct MeshComponent {
    Resource::LODMeshHandle lodMeshHandle;
    std::vector<Resource::MaterialHandle> materialSlots;  // [subMeshIndex] = MaterialHandle

    bool receivesShadow = true;

    Math::BoundingVolumeVariant localBounds;
    bool IsValid() const { return lodMeshHandle.IsValid(); }
};

// 地形组件
struct TerrainComponent {
    Resource::GeometryHandle geometryHandle;
    Resource::TextureHandle heightMapHandle;
    Resource::TextureHandle albedoHandle;
    Resource::TextureHandle normalHandle;

    float heightScale = 20.0f;
    float heightOffset = 0.0f;
    float tessellationFactor = 32.0f;
    float tessellationDistanceMin = 10.0f;
    float tessellationDistanceMax = 60.0f;

    uint32_t materialIndex = 0;
    Math::BoundingVolumeVariant localBounds;

    uint32_t constantBufferOffset = 0;
    bool needsUpload = true;

    bool IsValid() const { return geometryHandle.IsValid() && heightMapHandle.IsValid(); }
};

// 公告牌组件
enum class BillboardMode : uint8_t {
    AxisY,
    Full,
    Spherical
};

struct BillboardComponent {
    Resource::TextureHandle textureHandle;
    Resource::MaterialHandle materialHandle;

    float width = 2.0f;
    float height = 4.0f;
    BillboardMode mode = BillboardMode::AxisY;

    float minDistance = 10.0f;
    float maxDistance = 500.0f;
    float switchDistance = 50.0f;

    uint32_t textureArrayIndex = 0;

    bool IsValid() const { return textureHandle.IsValid(); }
};

} // namespace ECS
} // namespace DX12Engine
