#pragma once

#include "Math/BoundingVolume.h"
#include "Resource/Struct/GeometryHandle.h"
#include "Resource/Struct/MaterialHandle.h"
#include <DirectXMath.h>
#include <cstdint>
#include <d3d12.h>

namespace DX12Engine {

namespace Renderer {

// ============================================================================
// 渲染项 - 单个实体的绘制调用数据
// ============================================================================

struct RenderItem {

    Resource::GeometryHandle geometryHandle;
    Resource::MaterialHandle materialHandle;

    DirectX::XMMATRIX worldMatrix = {};     // 世界矩阵
    DirectX::XMMATRIX prevWorldMatrix = {}; // 上一帧世界矩阵（用于运动模糊）

    D3D12_GPU_VIRTUAL_ADDRESS objectCBAddress = 0;       // 世界矩阵等常量缓冲
    D3D12_GPU_VIRTUAL_ADDRESS materialCBAddress = 0;     // 材质参数缓冲（可选）
    D3D12_GPU_VIRTUAL_ADDRESS instanceBufferAddress = 0; // 实例化数据缓冲（可选）

    uint32_t objCBIndex = UINT32_MAX; // 常量缓冲区数组索引
    uint32_t materialCBIndex = UINT32_MAX;
    uint32_t numFramesDirty = 0; // 脏标记：指示数据需要更新到 GPU

    uint32_t instanceCount = 1;

    uint32_t indexCount = 0;
    uint32_t startIndexLocation = 0;
    uint32_t baseVertexLocation = 0;

    float depth = 0.0f; // 相机距离（用于排序）

    Math::BoundingVolumeVariant worldBounds; // 世界空间包围盒
    Math::BoundingSphere worldBoundsSphere;  // 世界空间包围球（用于快速剔除）

    bool isVisible = true;      // 是否可见（剔除结果）
    bool isTransparent = false; // 是否透明
    bool castsShadow = true;    // 是否投射阴影
    bool receivesShadow = true; // 是否接收阴影

    // ========================================================================
    // 排序键
    // ========================================================================
    // 位布局：
    //   Bits 63-56: 渲染队列类型（0=不透明，1=透明，2=UI等）
    //   Bits 55-48: 保留
    //   Bits 47-32: 材质 ID
    //   Bits 31-0:  深度值
    uint64_t sortKey = 0;

    // ========================================================================
    // 辅助方法
    // ========================================================================

    bool IsValid() const { return geometryHandle.IsValid() && materialHandle.IsValid(); }
    bool IsInstanced() const { return instanceCount > 1; }
    bool NeedSubMeshOverride() const { return indexCount > 0; }
    void BuildSortKey(uint8_t queueType, float depth) {
        uint64_t typePart = (static_cast<uint64_t>(queueType) & 0xFF) << 56;
        uint64_t materialPart = (static_cast<uint64_t>(materialHandle.GetHash()) & 0xFFFF) << 32;
        uint32_t depthInt = static_cast<uint32_t>(depth * 1000000.0f);
        sortKey = typePart | materialPart | depthInt;
    }
    void UpdateDirtyFlag() {
        if (numFramesDirty > 0) {
            numFramesDirty--;
        }
    }
    void MarkDirty() {
        numFramesDirty = 3; // 假设3帧缓冲
    }
};

} // namespace Renderer

} // namespace DX12Engine