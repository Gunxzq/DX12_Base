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
    D3D12_GPU_VIRTUAL_ADDRESS instanceBuffer; // InstanceData 数组 GPU 地址（FrameSync 填充）
    uint32_t instanceCount;                   // 实例数量（单物体=1）
    uint32_t probeIndex = UINT32_MAX;         // 反射探针索引 (UINT32_MAX = 无反射)
    uint32_t startIndex = 0;                  // 索引缓冲起始偏移（子网格）
    int32_t startVertex = 0;                  // 顶点缓冲起始偏移（子网格）
    uint32_t indexCount = 0;                  // 绘制索引数（0 = 使用 mesh->indexCount）
    uint32_t tempSlot = UINT32_MAX;           // 临时 InstanceData 槽位索引（FrameSync 解析）

    bool IsValid() const { return geometryHandle.IsValid() && instanceCount > 0; }

    // 工厂方法
    static OpaqueRenderItem Create(Resource::GeometryHandle geometry, uint32_t materialIdx,
                                   D3D12_GPU_VIRTUAL_ADDRESS instBuffer, uint32_t instCount,
                                   uint32_t probeIdx = UINT32_MAX, uint32_t startIdx = 0, int32_t startVtx = 0,
                                   uint32_t idxCount = 0) {
        OpaqueRenderItem item;
        item.geometryHandle = geometry;
        item.materialIndex = materialIdx;
        item.instanceBuffer = instBuffer;
        item.instanceCount = instCount;
        item.probeIndex = probeIdx;
        item.startIndex = startIdx;
        item.startVertex = startVtx;
        item.indexCount = idxCount;
        return item;
    }
};

} // namespace DX12Engine::Renderer
