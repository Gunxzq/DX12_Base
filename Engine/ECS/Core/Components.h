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

// TODO(StaticComponent): 静态实体持久化优化 — 暂未启用

// 设计目标：
//   对永不移动的实体（地形、建筑等），缓存 World 矩阵和实例数据到持久化
//   GPU 缓冲区，避免每帧重新上传，降低 CPU 开销。
//
// 当前问题（已禁用）：
//   1. OBJECT_DELETED_WHILE_STILL_IN_USE — GPU 还在使用时缓冲区被释放
//   2. 批次大小变化（实例数增加）时，旧持久化缓冲区太小导致越界
//   3. 多线程下 worldDirty 标志无同步，存在数据竞争
//   4. LOD / 剔除系统复用 cachedDistanceToCamera，相机移动时距离不更新
//
// 重新启用前需要：
//   - 用 GPU 围栏（Fence）确保持久化缓冲区安全释放
//   - 检测批次大小变化，自动重新分配更大的持久化缓冲区
//   - 为 worldDirty / cachedDistanceToCamera 加原子或锁保护
//   - 或改为完全在 RenderThread 更新，避免跨线程访问
struct StaticComponent {
    D3D12_GPU_VIRTUAL_ADDRESS persistentCBAddress = 0;       // 持久化常量缓冲区地址
    D3D12_GPU_VIRTUAL_ADDRESS persistentInstanceAddress = 0; // 持久化实例数据地址
    uint32_t persistentInstanceSize = 0;                     // 持久化实例缓冲区大小（字节）
    uint32_t batchInstanceIndex = UINT32_MAX;

    // TODO: 需要线程安全保护
    bool worldDirty = true;

    DirectX::XMFLOAT4X4 cachedWorld;             // 缓存的 World 矩阵
    DirectX::XMFLOAT4X4 cachedWorldInvTranspose; // 缓存的 WorldInvTranspose
    float cachedDistanceToCamera = 0.0f;         // 缓存的到相机距离（LOD 用）
};

// PickingComponent — 标记实体可被拾取 + 拾取状态
struct PickingComponent {
    bool isPickable = true;               // 是否可以被拾取（总开关）
    int32_t priority = 0;                 // 拾取优先级（数值越高，同射线下优先被选中）
    uint32_t pickableBy = 0;              // 谁可以拾取（位掩码：玩家、AI、编辑器等）
    bool enableHighlight = true;          // 是否显示高亮效果
    uint32_t highlightColor = 0xFFFFFFFF; // 高亮颜色（RGBA）
    bool editableInEditor = true;         // 编辑器中是否可被选中
    bool showBoundingBox = false;         // 调试：显示包围盒
};

// 点位置组件
struct PositionComponent {
    DirectX::XMFLOAT3 position;
};

// 静态绑定反射探针组件
struct ReflectionConsumerComponent {
    uint32_t probeIndex = UINT32_MAX; // 静态绑定的探针索引
    bool useDynamicFallback = true;   // 如果 probeIndex 无效，是否回退到动态查询
};

} // namespace ECS
} // namespace DX12Engine