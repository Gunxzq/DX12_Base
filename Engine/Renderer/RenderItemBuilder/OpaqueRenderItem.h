#pragma once

#include "Renderer/FrameResources/Struct/FrameResourceTypes.h"
#include "Resource/Struct/GeometryHandle.h"
#include <DirectXMath.h>
#include <d3d12.h>

namespace DX12Engine::Renderer {

// ============================================================================
// 渲染项 - 支持单物体和实例化两种模式
// ============================================================================
struct OpaqueRenderItem {
    enum class Type : uint8_t {
        Standard, // 单物体模式：使用独立的 CBV
        Instanced // 实例化模式：使用 InstanceData StructuredBuffer
    } type;

    // 公共数据
    Resource::GeometryHandle geometryHandle;
    uint32_t materialIndex;
    D3D12_GPU_DESCRIPTOR_HANDLE textureSRV;

    // 模式专用数据
    union {
        struct {
            D3D12_GPU_VIRTUAL_ADDRESS constantBuffer; // 单物体模式：ObjectConstants CBV
        } standard;

        struct {
            D3D12_GPU_VIRTUAL_ADDRESS instanceBuffer; // 实例化模式：InstanceData 数组
            uint32_t instanceCount;                   // 实例数量
        } instanced;
    };

    // 辅助方法
    bool IsStandard() const { return type == Type::Standard; }
    bool IsInstanced() const { return type == Type::Instanced; }
    bool IsValid() const { return geometryHandle.IsValid(); }

    // 工厂方法
    static OpaqueRenderItem CreateStandard(Resource::GeometryHandle geometry, uint32_t materialIdx,
                                           D3D12_GPU_DESCRIPTOR_HANDLE texture, D3D12_GPU_VIRTUAL_ADDRESS cbAddress) {
        OpaqueRenderItem item;
        item.type = Type::Standard;
        item.geometryHandle = geometry;
        item.materialIndex = materialIdx;
        item.textureSRV = texture;
        item.standard.constantBuffer = cbAddress;
        return item;
    }

    static OpaqueRenderItem CreateInstanced(Resource::GeometryHandle geometry, uint32_t materialIdx,
                                            D3D12_GPU_DESCRIPTOR_HANDLE texture, D3D12_GPU_VIRTUAL_ADDRESS instBuffer,
                                            uint32_t instCount) {
        OpaqueRenderItem item;
        item.type = Type::Instanced;
        item.geometryHandle = geometry;
        item.materialIndex = materialIdx;
        item.textureSRV = texture;
        item.instanced.instanceBuffer = instBuffer;
        item.instanced.instanceCount = instCount;
        return item;
    }
};

} // namespace DX12Engine::Renderer