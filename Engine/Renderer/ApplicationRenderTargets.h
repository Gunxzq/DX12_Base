#pragma once

#include "Common/d3dUtil.h"
#include "Resource/Core/DescriptorHeapCollection.h"
#include "Resource/Struct/DescriptorHandle.h"
#include <wrl/client.h>

namespace DX12Engine::Resource {
class DescriptorHeapCollection;
class RenderTargetPool;
} // namespace DX12Engine::Resource

namespace DX12Engine::Renderer {

// ========================================================================
// ApplicationRenderTargets — 视口帧缓冲管理器
// 拥有随窗口缩放的临时 RT，统一处理分配/重建/查询
// ========================================================================
class ApplicationRenderTargets {
public:
    ApplicationRenderTargets() = default;
    ~ApplicationRenderTargets();

    ApplicationRenderTargets(const ApplicationRenderTargets &) = delete;
    ApplicationRenderTargets &operator=(const ApplicationRenderTargets &) = delete;

    void Initialize(ID3D12Device *device, Resource::DescriptorHeapCollection *heaps, uint32_t width, uint32_t height,
                    Resource::HeapTag heapTag = Resource::HeapTag::Default);
    void OnResize(uint32_t width, uint32_t height);
    void Shutdown();

    bool IsInitialized() const { return m_initialized; }

    // ── G-buffer 句柄访问 ──

    const Resource::RenderTargetHandle &GetGBufferAlbedo() const { return m_gbuffer.albedo; }
    const Resource::RenderTargetHandle &GetGBufferNormal() const { return m_gbuffer.normal; }
    const Resource::RenderTargetHandle &GetGBufferMaterial() const { return m_gbuffer.material; }
    const Resource::RenderTargetHandle &GetGBufferWorldPos() const { return m_gbuffer.worldPos; }

    // ── 场景颜色 RT（光照 Pass 输出，天空盒/网格共用） ──

    const Resource::RenderTargetHandle &GetSceneColor() const { return m_sceneColor; }
    ID3D12Resource *GetSceneColorResource() const;
    D3D12_GPU_DESCRIPTOR_HANDLE GetSceneColorSRV() const;

    // ── G-buffer GPU SRV 句柄（直接绑定到光照 Pass） ──

    D3D12_GPU_DESCRIPTOR_HANDLE GetGBufferAlbedoSRV() const;
    D3D12_GPU_DESCRIPTOR_HANDLE GetGBufferNormalSRV() const;
    D3D12_GPU_DESCRIPTOR_HANDLE GetGBufferMaterialSRV() const;
    D3D12_GPU_DESCRIPTOR_HANDLE GetGBufferWorldPosSRV() const;

    // ── G-buffer 资源指针（用于屏障过渡） ──

    ID3D12Resource *GetGBufferAlbedoResource() const;
    ID3D12Resource *GetGBufferNormalResource() const;
    ID3D12Resource *GetGBufferMaterialResource() const;
    ID3D12Resource *GetGBufferWorldPosResource() const;

private:
    void AllocateGBuffer(uint32_t width, uint32_t height);
    void FreeGBuffer();

    struct {
        Resource::RenderTargetHandle albedo;
        Resource::RenderTargetHandle normal;
        Resource::RenderTargetHandle material;
        Resource::RenderTargetHandle worldPos;
    } m_gbuffer;

    Resource::RenderTargetHandle m_sceneColor;

    ID3D12Device *m_device = nullptr;
    Resource::DescriptorHeapCollection *m_heaps = nullptr;
    Resource::HeapTag m_heapTag = Resource::HeapTag::Default;
    bool m_initialized = false;
};

} // namespace DX12Engine::Renderer
