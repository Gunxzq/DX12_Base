#pragma once
#include "Renderer/Material/MaterialHandle.h"
#include "Renderer/PipelineLayer/ShaderRoute.h"
#include "Resource/Geometry/TriangleMesh.h"
#include "Resource/Struct/GeometryHandle.h"
#include "Resource/Struct/LODMeshHandle.h"
#include "Resource/Struct/TextureHandle.h"

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

    bool receivesShadow = false; // 接收阴影（光照阶段采样阴影贴图）
    bool castsShadow = false;    // 投射阴影（进入阴影剔除 → 阴影贴图渲染；地形/树/山可关闭）

    Math::BoundingVolumeVariant localBounds;
    bool IsValid() const { return lodMeshHandle.IsValid(); }
};

// 子网格区间（该材质覆盖的索引段）
struct SubMeshRange {
    uint32_t startIndex = 0;
    uint32_t indexCount = 0;
};

// 单个渲染槽位（材质句柄 + 覆盖的子网格区间 + 渲染器标记）
struct RenderSlot {
    Resource::MaterialHandle material;                               // 材质句柄
    std::vector<SubMeshRange> subMeshRanges;                         // 该材质覆盖的子网格区间
    Renderer::ShaderType shaderType = Renderer::ShaderType::Unknown; // 渲染器标记（路由键）
    bool IsValid() const { return material.IsValid(); }
};

// 通用渲染槽位组件（一个组件，不分渲染器类型）
struct RenderSlotComponent {
    std::vector<RenderSlot> slots; // 本实体全部渲染槽位
    bool IsValid() const { return !slots.empty(); }
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
enum class BillboardMode : uint8_t { AxisY, Full, Spherical, XCross };

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
