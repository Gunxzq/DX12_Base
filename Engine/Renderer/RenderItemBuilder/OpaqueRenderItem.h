#pragma once

#include "Renderer/FrameResources/Struct/FrameResourceTypes.h"
#include "Resource/Struct/GeometryHandle.h"
#include <DirectXMath.h>
#include <d3d12.h>

namespace DX12Engine::Renderer {

// ============================================================================
// 渲染项 - 统一实例化模式（单物体 instanceCount=1）
// ============================================================================
struct OpaqueRenderItem {

    Resource::GeometryHandle geometryHandle;
    uint32_t materialIndex;
    D3D12_GPU_DESCRIPTOR_HANDLE textureSRV;
    D3D12_GPU_VIRTUAL_ADDRESS instanceBuffer; // InstanceData 数组 GPU 地址
    uint32_t instanceCount;                   // 实例数量（单物体=1）
    uint32_t probeIndex = UINT32_MAX;         // 反射探针索引 (UINT32_MAX = 无反射)

    bool IsValid() const { return geometryHandle.IsValid() && instanceBuffer != 0 && instanceCount > 0; }

    // 工厂方法
    static OpaqueRenderItem Create(Resource::GeometryHandle geometry, uint32_t materialIdx,
                                   D3D12_GPU_DESCRIPTOR_HANDLE texture, D3D12_GPU_VIRTUAL_ADDRESS instBuffer,
                                   uint32_t instCount, uint32_t probeIdx = UINT32_MAX) {
        OpaqueRenderItem item;
        item.geometryHandle = geometry;
        item.materialIndex = materialIdx;
        item.textureSRV = texture;
        item.instanceBuffer = instBuffer;
        item.instanceCount = instCount;
        item.probeIndex = probeIdx;
        return item;
    }
};

} // namespace DX12Engine::Renderer
