#pragma once

#include "Common/d3dUtil.h"
#include "Renderer/Material/MaterialHandle.h"
#include "Resource/Core/GpuHandlePool.h"
#include "Resource/Struct/Descriptor.h"
#include "Resource/Struct/DescriptorHandle.h"
#include "Resource/Struct/GeometryHandle.h"
#include "Resource/Struct/TextureHandle.h"
#include <cstdint>
#include <vector>



using PreviewId = uint32_t; // 预览上下文 ID

enum class PreviewType : uint8_t {
    Detail,   ///< 大预览（独立 RT，由 RenderTargetPool 分配）
    Thumbnail ///< 缩略图（共享纹理数组，由 ThumbnailArray 管理 slice）
};

/// 预览渲染模式
enum class PreviewRenderMode : uint8_t {
    PBR,   ///< PBR 光照渲染（材质预览用）
    Unlit  ///< 无光照直接采样（纹理预览用）
};

/**
 * @brief 预览上下文 — 单个预览实例的数据切片
 *
 * 包含独立相机、预览实体、离屏 RT/纹理数组 slice 及输出 SRV。
 * 由 PreviewManager 统一管理生命周期，特化层 Panel 持有 PreviewId。
 */
struct PreviewContext {
    // ========================================================================
    // 类型与序列号
    // ========================================================================
    PreviewType type = PreviewType::Detail;
    PreviewRenderMode renderMode = PreviewRenderMode::PBR; // 渲染模式
    uint64_t loadSequence = 0;        // 当前数据对应的加载序列号
    uint64_t pendingLoadSequence = 0; // 正在进行的异步加载序列号（可能 > loadSequence）

    // ========================================================================
    // 相机
    // ========================================================================
    DirectX::XMFLOAT3 position = {3.0f, 2.0f, 3.0f};
    DirectX::XMFLOAT3 target = {0.0f, 0.0f, 0.0f};
    float fov = DirectX::XMConvertToRadians(45.0f);
    float nearPlane = 0.1f;
    float farPlane = 100.0f;

    // ========================================================================
    // 方向光
    // ========================================================================
    DirectX::XMFLOAT3 lightDirection = {0.0f, -1.0f, 0.0f}; // 从上往下
    float lightStrength = 3.0f;

    // ========================================================================
    // 预览网格
    // ========================================================================
    DX12Engine::Resource::GpuResourceHandle meshVB = DX12Engine::Resource::GpuResourceHandle::Invalid();
    DX12Engine::Resource::GpuResourceHandle meshIB = DX12Engine::Resource::GpuResourceHandle::Invalid();
    uint32_t indexCount = 0;                        // 索引数量
    uint32_t vertexCount = 0;                       // 顶点数量
    uint32_t vertexStride = 0;                      // 顶点步长
    DXGI_FORMAT indexFormat = DXGI_FORMAT_R16_UINT; // 索引格式

    // ========================================================================
    // 材质参数
    // ========================================================================
    DirectX::XMFLOAT4 baseColor = {0.5f, 0.5f, 0.5f, 1.0f}; // 默认灰色

    /// GeometryResourceManager 中的几何体句柄（用于释放）
    DX12Engine::Resource::GeometryHandle geometryHandle;

    /// 预览纹理句柄（纹理预览用，TextureManager 管理生命周期）
    DX12Engine::Resource::TextureHandle previewTexture;

    /// 预览材质句柄（材质预览用，MaterialManager 管理生命周期）
    DX12Engine::Resource::MaterialHandle previewMaterial;

    // ========================================================================
    // 离屏渲染目标（Detail 模式使用）
    // ========================================================================
    DX12Engine::Resource::RenderTargetHandle renderTarget;  // 从 RenderTargetPool 分配
    D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = {}; // RTV 句柄
    D3D12_CPU_DESCRIPTOR_HANDLE dsvHandle = {}; // DSV 句柄

    // ========================================================================
    // 共享纹理数组（Thumbnail 模式使用）
    // ========================================================================
    uint32_t arraySlice = UINT32_MAX; // 在 ThumbnailArray 中的层索引

    // ========================================================================
    // 输出 SRV（ImGui 堆，供 ImGui::Image 使用）
    // ========================================================================
    D3D12_GPU_DESCRIPTOR_HANDLE outputSRV = {};   // GPU 句柄
    D3D12_CPU_DESCRIPTOR_HANDLE imguiSrvCpu = {}; // CPU 句柄（用于释放）

    // ========================================================================
    // 状态
    // ========================================================================
    bool needsRender = true; // 是否需要重新渲染
    bool valid = false;      // 上下文是否有效
    bool visible = true;     // 是否可见

    // ========================================================================
    // 尺寸
    // ========================================================================
    uint32_t width = 256;
    uint32_t height = 256;

    /// 动态视口尺寸（每帧由 DrawPreview 更新），0 表示回退到 width/height
    uint32_t viewportWidth = 0;
    uint32_t viewportHeight = 0;
};

