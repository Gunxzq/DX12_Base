#include "ApplicationRenderTargets.h"
#include "Resource/Core/DescriptorHeapCollection.h"
#include "Resource/Pool/RenderTargetPool.h"

using namespace DX12Engine::Renderer;

ApplicationRenderTargets::~ApplicationRenderTargets() { Shutdown(); }

void ApplicationRenderTargets::Initialize(ID3D12Device *device, Resource::DescriptorHeapCollection *heaps,
                                          uint32_t width, uint32_t height, Resource::HeapTag heapTag) {
    if (m_initialized)
        Shutdown();

    m_device = device;
    m_heaps = heaps;
    m_heapTag = heapTag;

    AllocateGBuffer(width, height);
    m_initialized = true;
}

void ApplicationRenderTargets::Shutdown() {
    if (!m_initialized)
        return;
    FreeGBuffer();
    m_initialized = false;
    m_device = nullptr;
    m_heaps = nullptr;
}

void ApplicationRenderTargets::OnResize(uint32_t width, uint32_t height) {
    if (!m_initialized)
        return;
    FreeGBuffer();
    AllocateGBuffer(width, height);
}

// ========================================================================
// G-buffer 分配/释放
// ========================================================================

void ApplicationRenderTargets::AllocateGBuffer(uint32_t width, uint32_t height) {
    auto &rtPool = Resource::RenderTargetPool::GetInstance();

    auto makeDesc = [&](DXGI_FORMAT fmt, const std::wstring &name) {
        Resource::RenderTargetDesc d;
        d.width = width;
        d.height = height;
        d.format = fmt;
        d.clearValue.Format = fmt;
        d.clearValue.Color[0] = 0.0f;
        d.clearValue.Color[1] = 0.0f;
        d.clearValue.Color[2] = 0.0f;
        d.clearValue.Color[3] = 0.0f;
        d.name = name;
        return d;
    };

    m_gbuffer.albedo = rtPool.Allocate(makeDesc(DXGI_FORMAT_R8G8B8A8_UNORM, L"Gbuffer_Albedo"), m_heapTag);
    m_gbuffer.normal = rtPool.Allocate(makeDesc(DXGI_FORMAT_R16G16B16A16_FLOAT, L"Gbuffer_Normal"), m_heapTag);
    m_gbuffer.material = rtPool.Allocate(makeDesc(DXGI_FORMAT_R8G8B8A8_UNORM, L"Gbuffer_Material"), m_heapTag);
    m_gbuffer.worldPos = rtPool.Allocate(makeDesc(DXGI_FORMAT_R16G16B16A16_FLOAT, L"Gbuffer_WorldPos"), m_heapTag);

    m_sceneColor = rtPool.Allocate(makeDesc(DXGI_FORMAT_R8G8B8A8_UNORM, L"SceneColor"), m_heapTag);
}

void ApplicationRenderTargets::FreeGBuffer() {
    auto &rtPool = Resource::RenderTargetPool::GetInstance();
    uint64_t fence = UINT64_MAX; // 立即释放

    auto freeIfValid = [&](Resource::RenderTargetHandle &h) {
        if (h.IsValid()) {
            rtPool.Free(h, fence);
            h = {};
        }
    };

    freeIfValid(m_gbuffer.albedo);
    freeIfValid(m_gbuffer.normal);
    freeIfValid(m_gbuffer.material);
    freeIfValid(m_gbuffer.worldPos);
    freeIfValid(m_sceneColor);
}

// ========================================================================
// G-buffer SRV GPU 句柄
// ========================================================================

D3D12_GPU_DESCRIPTOR_HANDLE ApplicationRenderTargets::GetGBufferAlbedoSRV() const {
    return Resource::RenderTargetPool::GetInstance().GetSrvGpuHandle(m_gbuffer.albedo);
}
D3D12_GPU_DESCRIPTOR_HANDLE ApplicationRenderTargets::GetGBufferNormalSRV() const {
    return Resource::RenderTargetPool::GetInstance().GetSrvGpuHandle(m_gbuffer.normal);
}
D3D12_GPU_DESCRIPTOR_HANDLE ApplicationRenderTargets::GetGBufferMaterialSRV() const {
    return Resource::RenderTargetPool::GetInstance().GetSrvGpuHandle(m_gbuffer.material);
}
D3D12_GPU_DESCRIPTOR_HANDLE ApplicationRenderTargets::GetGBufferWorldPosSRV() const {
    return Resource::RenderTargetPool::GetInstance().GetSrvGpuHandle(m_gbuffer.worldPos);
}

// ========================================================================
// G-buffer 资源指针（屏障用）
// ========================================================================

ID3D12Resource *ApplicationRenderTargets::GetGBufferAlbedoResource() const {
    return Resource::RenderTargetPool::GetInstance().GetResource(m_gbuffer.albedo);
}
ID3D12Resource *ApplicationRenderTargets::GetGBufferNormalResource() const {
    return Resource::RenderTargetPool::GetInstance().GetResource(m_gbuffer.normal);
}
ID3D12Resource *ApplicationRenderTargets::GetGBufferMaterialResource() const {
    return Resource::RenderTargetPool::GetInstance().GetResource(m_gbuffer.material);
}
ID3D12Resource *ApplicationRenderTargets::GetGBufferWorldPosResource() const {
    return Resource::RenderTargetPool::GetInstance().GetResource(m_gbuffer.worldPos);
}

// ========================================================================
// 场景颜色 RT
// ========================================================================

ID3D12Resource *ApplicationRenderTargets::GetSceneColorResource() const {
    return Resource::RenderTargetPool::GetInstance().GetResource(m_sceneColor);
}

D3D12_GPU_DESCRIPTOR_HANDLE ApplicationRenderTargets::GetSceneColorSRV() const {
    return Resource::RenderTargetPool::GetInstance().GetSrvGpuHandle(m_sceneColor);
}
