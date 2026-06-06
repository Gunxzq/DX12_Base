#pragma once
#include "Math/BoundingVolume.h"
#include "Resource/Struct/GeometryHandle.h"
#include <DirectXMath.h>
#include <cstdint>
#include <d3d12.h>

namespace DX12Engine::Renderer {

// ============================================================================
// 地形块标识符
// ============================================================================
struct TerrainBlockId {
    uint32_t index = UINT32_MAX;
    uint32_t generation = 0;

    bool IsValid() const { return index != UINT32_MAX; }
    bool operator==(const TerrainBlockId &other) const {
        return index == other.index && generation == other.generation;
    }
};

// ============================================================================
// 地形常量（每块，每帧更新）
// ============================================================================
struct TerrainConstants {
    DirectX::XMFLOAT4X4 World;             // gWorld
    DirectX::XMFLOAT4X4 WorldInvTranspose; // gWorldInvTrans
    DirectX::XMFLOAT4X4 PrevWorld;         // gPrevWorld (vs color.hlsl cbPerObject)
    uint32_t MaterialIndex;                // gMaterialIndex
    uint32_t ReceiveShadow;                // gReceiveShadow
    float ObjPad[2];                       // gObjPad (16 bytes aligned)
    float HeightScale;                     // 高度缩放
    float HeightOffset;                    // 高度偏移
    float TessellationFactor;              // 基础细分因子
    float TessellationDistanceMin;         // 最小细分距离
    float TessellationDistanceMax;         // 最大细分距离
    uint32_t HeightMapIndex;               // 高度图纹理索引
    uint32_t AlbedoMapIndex;               // 漫反射纹理索引
    uint32_t NormalMapIndex;               // 法线贴图索引
    float TerrainPad;                      // gTerrainPad
    float pad[3];
};

// ============================================================================
// 地形块运行时数据
// ============================================================================
struct TerrainBlockRuntime {
    TerrainBlockId id;
    DX12Engine::Resource::GeometryHandle geometryHandle; // 指向 PatchMesh
    DX12Engine::Math::BoundingVolumeVariant bounds;      // 包围盒
    TerrainConstants constants;                          // CPU 端镜像
    D3D12_GPU_VIRTUAL_ADDRESS gpuAddress;                // GPU 常量缓冲区地址
    bool isActive = true;
    float pad[3];
};

} // namespace DX12Engine::Renderer