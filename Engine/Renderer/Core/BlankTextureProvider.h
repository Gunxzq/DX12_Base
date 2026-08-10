#pragma once

#include <cstdint>
#include <d3d12.h>
#include <wrl/client.h>

#include "Resource/Core/DescriptorHeapCollection.h" // HeapTag / PartitionType（接口参数需完整定义）
#include "Resource/Core/GpuHandlePool.h"            // GpuResourceHandle（值类型成员，需完整定义）

namespace DX12Engine::Resource {
class DescriptorHeapCollection;
class GpuResourceManager;
} // namespace DX12Engine::Resource

namespace DX12Engine::Renderer {

class CommandManager;

// ============================================================================
// BlankTextureProvider — 引擎 CORE 空白/占位纹理提供者（Game/Editor 共用单例）
//
// 背景：多个管理器（AmbientOcclusionManager / SkyboxManager / WaterManager 等）
//   在对应资源尚未加载时，shader 侧仍可能访问纹理根参数（lighting.hlsl 采样
//   gSsaoMap/gEnvMap、water.hlsl 采样 gEnvMap）。若传空句柄导致根参数未绑定，
//   GBV #935 报未初始化根参数（不开 GBV 时可能产生无效描述符句柄崩溃 #646）。
//   解决方案：管理器初始化前，由本提供者同步阻塞创建空白纹理并注入管理器。
//
// 格式/颜色按管理器需求：
//   - White2D   ：1x1 R8G8B8A8_UNORM（0xFFFFFFFF）→ SSAO fallback（采样 .r = 1.0，无遮蔽）
//   - BlackCube ：1x1x6 R8G8B8A8_UNORM（全 0x00）  → 环境贴图 fallback（无天空盒时反射为黑）
//
// 同步语义：Initialize 内部上传 + FlushCommandQueue 阻塞至 GPU 完成，
//   保证返回的 SRV 在后续任意管理器 Initialize 时即可使用。
// ============================================================================

class BlankTextureProvider {
public:
    static BlankTextureProvider &GetInstance();

    BlankTextureProvider(const BlankTextureProvider &) = delete;
    BlankTextureProvider &operator=(const BlankTextureProvider &) = delete;

    BlankTextureProvider() = default;
    ~BlankTextureProvider() = default;

    // ---- 生命周期 ----
    /// 同步创建白色 2D + 黑色 Cubemap 并上传（阻塞至 GPU 完成）
    /// @param heapTag 纹理 SRV 槽位所在堆域（编辑器传 EditorViewport，Game 传 Default）
    void Initialize(ID3D12Device *device, Resource::DescriptorHeapCollection *descHeaps, CommandManager *cmdMgr,
                    Resource::HeapTag heapTag = Resource::HeapTag::Default);
    void Shutdown();

    // ---- 空白纹理访问 ----
    /// 1x1 白色 2D SRV（SSAO fallback，采样得 1.0）
    D3D12_GPU_DESCRIPTOR_HANDLE GetWhite2DSRV() const { return m_white2DSRV; }
    /// 1x1x6 黑色 Cubemap SRV（环境贴图 fallback，反射为黑）
    D3D12_GPU_DESCRIPTOR_HANDLE GetBlackCubeSRV() const { return m_blackCubeSRV; }

    bool IsInitialized() const { return m_initialized; }

private:
    /// 创建并上传 1x1 白色 2D 纹理（R8G8B8A8_UNORM 0xFFFFFFFF）+ SRV
    void CreateWhite2D(ID3D12Device *device, Resource::DescriptorHeapCollection *descHeaps, CommandManager *cmdMgr,
                       Resource::HeapTag heapTag);
    /// 创建并上传 1x1x6 黑色 Cubemap（R8G8B8A8_UNORM 全 0x00）+ SRV
    void CreateBlackCube(ID3D12Device *device, Resource::DescriptorHeapCollection *descHeaps, CommandManager *cmdMgr,
                         Resource::HeapTag heapTag);

private:
    ID3D12Device *m_device = nullptr;
    bool m_initialized = false;

    // 白色 2D 纹理（Texture 分区 SRV）
    Resource::GpuResourceHandle m_whiteTexHandle;
    uint32_t m_whiteSrvSlot = UINT32_MAX;
    D3D12_GPU_DESCRIPTOR_HANDLE m_white2DSRV = {};

    // 黑色 Cubemap（Texture 分区 SRV）
    Resource::GpuResourceHandle m_blackCubeHandle;
    uint32_t m_blackCubeSrvSlot = UINT32_MAX;
    D3D12_GPU_DESCRIPTOR_HANDLE m_blackCubeSRV = {};
};

} // namespace DX12Engine::Renderer
