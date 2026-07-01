#pragma once

#include "Renderer/FrameResources/Struct/FrameResourceTypes.h"
#include "Resource/Struct/GeometryHandle.h"
#include <DirectXMath.h>
#include <d3d12.h>

namespace DX12Engine::Renderer {

// ============================================================================
// 蒙皮渲染项 — 走独立 SkinnedRenderer，带骨骼缓冲区地址
// ============================================================================
struct SkinnedRenderItem {

    Resource::GeometryHandle geometryHandle;
    uint32_t materialIndex;
    D3D12_GPU_VIRTUAL_ADDRESS instanceBuffer;    // InstanceData 数组 GPU 地址
    D3D12_GPU_VIRTUAL_ADDRESS boneBufferAddress; // 骨骼矩阵 GPU 地址
    uint32_t instanceCount;                      // 实例数量（单物体=1）
    uint32_t indexCount = 0;                     // 子集索引数量（非完整 mesh 的索引数）
    uint32_t startIndex = 0;                     // 索引起始偏移（子网格）
    int32_t startVertex = 0;                     // 顶点起始偏移（子网格）

    bool IsValid() const {
        return geometryHandle.IsValid() && instanceBuffer != 0 && boneBufferAddress != 0 && instanceCount > 0;
    }

    static SkinnedRenderItem Create(Resource::GeometryHandle geometry, uint32_t materialIdx,
                                    D3D12_GPU_VIRTUAL_ADDRESS instBuffer, D3D12_GPU_VIRTUAL_ADDRESS boneBufferAddr,
                                    uint32_t instCount, uint32_t idxCount = 0, uint32_t startIdx = 0,
                                    int32_t startVtx = 0) {
        SkinnedRenderItem item;
        item.geometryHandle = geometry;
        item.materialIndex = materialIdx;
        item.instanceBuffer = instBuffer;
        item.boneBufferAddress = boneBufferAddr;
        item.instanceCount = instCount;
        item.indexCount = idxCount;
        item.startIndex = startIdx;
        item.startVertex = startVtx;
        return item;
    }
};

} // namespace DX12Engine::Renderer
