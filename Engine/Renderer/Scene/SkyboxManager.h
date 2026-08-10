#pragma once

#include "Resource/Core/DescriptorHeapCollection.h"
#include "Resource/GpuResourceManager.h"
#include "Resource/Manager/GeometryResourceManager.h"
#include "Resource/Struct/GeometryHandle.h"
#include <d3d12.h>

namespace DX12Engine::Renderer {

// ============================================================================
// SkyboxManager — 天空盒管理器（单例）
//
// 架构模式对齐 LightManager / ReflectionProbeManager：
//   - 单例，脱离 ECS / Builder / FrameDriver 帧生命周期
//   - 自管 GPU 资源（Cubemap SRV、持久 UPLOAD CB）
//   - 任意时刻调用 SetSkybox() 切换天空盒
//   - 渲染系统直接读取 GetXXX() 接口获取数据
//
// 使用场景：
//   运行时：SceneConstructor 完成资源加载后调用 SetSkybox
//   开放世界：昼夜/天气系统切换时再次调用 SetSkybox
//   编辑器：运行时替换天空盒
// ============================================================================

class SkyboxManager {
public:
    static SkyboxManager &GetInstance();

    SkyboxManager(const SkyboxManager &) = delete;
    SkyboxManager &operator=(const SkyboxManager &) = delete;

    SkyboxManager() = default;
    ~SkyboxManager() = default;

    // ========================================================================
    // 生命周期
    // ========================================================================

    void Initialize(ID3D12Device *device, Resource::DescriptorHeapCollection *descHeaps,
                    Resource::HeapTag heapTag = Resource::HeapTag::Default);
    void Shutdown();

    // ========================================================================
    // 天空盒设置（资源就绪后任意时刻调用）
    // ========================================================================

    /// 设置天空盒资源
    /// @param textureResource  立方体贴图 GPU 资源（由 AssetManager 异步加载）
    /// @param geometryHandle   天空盒网格句柄（通常为单位盒/球）
    void SetSkybox(Resource::GpuResourceHandle textureResource, Resource::GeometryHandle geometryHandle);

    /// 注入空白 Cubemap fallback（由引擎 CORE BlankTextureProvider 提供，初始化时同步创建）。
    /// 无真实天空盒时 GetCubeSRV() 返回该 fallback，避免 shader 访问未绑定根参数（GBV #935）。
    void SetFallbackCubeSRV(D3D12_GPU_DESCRIPTOR_HANDLE srv) { m_fallbackCubeSRV = srv; }

    /// 清空天空盒（退回到纯色/默认）
    void ClearSkybox();

    // ========================================================================
    // 数据访问（供 SkyboxRenderSystem 使用）
    // ========================================================================

    bool IsValid() const {
        return m_textureResource.IsValid() && m_geometryHandle.IsValid() && m_cubeSrvIndex != UINT32_MAX &&
               m_objectCBAddress != 0;
    }

    Resource::GeometryHandle GetGeometry() const { return m_geometryHandle; }

    Resource::GpuResourceHandle GetTextureResource() const { return m_textureResource; }
    /// 环境贴图 SRV：真实天空盒有效时返回 Cubemap SRV，否则返回空句柄
    /// （空句柄时由 lighting.hlsl 的 if 分支跳过 gEnvMap 采样，避免根参数未初始化）
    D3D12_GPU_DESCRIPTOR_HANDLE GetCubeSRV() const;
    D3D12_GPU_VIRTUAL_ADDRESS GetObjectCBAddress() const { return m_objectCBAddress; }

private:
    /// 在 DescriptorHeap 中创建 Cubemap SRV
    void CreateCubeSRV();

    /// 分配持久 UPLOAD 堆 ObjectConstants CB（单位矩阵）
    void AllocateObjectCB();

private:
    ID3D12Device *m_device = nullptr;
    Resource::DescriptorHeapCollection *m_descHeaps = nullptr;
    Resource::HeapTag m_heapTag = Resource::HeapTag::Default;
    bool m_initialized = false;

    // 天空盒资源
    Resource::GpuResourceHandle m_textureResource; // 立方体贴图原始 GPU 资源
    Resource::GeometryHandle m_geometryHandle;     // 天空盒网格
    uint32_t m_cubeSrvIndex = UINT32_MAX;          // TEXTURECUBE SRV 槽位

    // 空白 Cubemap fallback（引擎 CORE BlankTextureProvider 注入；无真实天空盒时 GetCubeSRV 返回）
    D3D12_GPU_DESCRIPTOR_HANDLE m_fallbackCubeSRV = {};

    // 持久 ObjectConstants CB（UPLOAD 堆，贯穿生命周期）
    D3D12_GPU_VIRTUAL_ADDRESS m_objectCBAddress = 0;
    Resource::GpuResourceHandle m_cbBuffer; // UPLOAD 缓冲句柄
};

} // namespace DX12Engine::Renderer
