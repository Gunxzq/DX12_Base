#include "WindowFrameResources.h"
#include "Logger/Logger.h"

using namespace DX12Engine::Renderer;

// ========================================================================
// 构造 / 析构
// ========================================================================

WindowFrameResources::~WindowFrameResources() { Shutdown(); }

// ========================================================================
// Initialize / Shutdown
// ========================================================================

void WindowFrameResources::Initialize(ID3D12Device *device, Resource::DescriptorHeapCollection *heaps, uint32_t width,
                                      uint32_t height, Resource::HeapTag heapTag, const Desc &desc) {
    if (m_initialized)
        Shutdown();

    m_device = device;
    m_heaps = heaps;
    m_heapTag = heapTag;
    m_desc = desc;

    AllocateResources(width, height);
    m_initialized = true;

    Logger::Logger::GetInstance()->Info("[WindowFrameResources] Initialized: {}x{} (tag={})", width, height,
                                        static_cast<int>(heapTag));
}

void WindowFrameResources::Shutdown() {
    if (!m_initialized)
        return;
    FreeResources();
    m_initialized = false;
    m_device = nullptr;
    m_heaps = nullptr;
    Logger::Logger::GetInstance()->Info("[WindowFrameResources] Shutdown");
}

// ========================================================================
// OnResize
// ========================================================================

void WindowFrameResources::OnResize(uint32_t width, uint32_t height) {
    if (!m_initialized)
        return;
    if (width == 0 || height == 0)
        return;
    if (width == m_width && height == m_height)
        return;

    Logger::Logger::GetInstance()->Info("[WindowFrameResources] OnResize: {}x{} (was {}x{})", width, height, m_width,
                                        m_height);

    FreeResources();
    AllocateResources(width, height);

    Logger::Logger::GetInstance()->Info("[WindowFrameResources] OnResize complete: {}x{}", m_width, m_height);
}

// ========================================================================
// 资源分配 / 释放
// ========================================================================

void WindowFrameResources::AllocateResources(uint32_t width, uint32_t height) {
    Logger::Logger::GetInstance()->Info("[WindowFrameResources] AllocateResources: {}x{}", width, height);

    CD3DX12_HEAP_PROPERTIES defaultHeap(D3D12_HEAP_TYPE_DEFAULT);

    // ── 1. G-buffer ×4 ──
    static constexpr const wchar_t *kGBufNames[4] = {
        L"WindowFrameResources_GBufferAlbedo",
        L"WindowFrameResources_GBufferNormal",
        L"WindowFrameResources_GBufferMaterial",
        L"WindowFrameResources_GBufferWorldPos",
    };

    for (int i = 0; i < 4; ++i) {
        D3D12_RESOURCE_DESC resDesc = {};
        resDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        resDesc.Width = width;
        resDesc.Height = height;
        resDesc.DepthOrArraySize = 1;
        resDesc.MipLevels = 1;
        resDesc.Format = m_desc.gbufferFormats[i];
        resDesc.SampleDesc.Count = 1;
        resDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

        D3D12_CLEAR_VALUE clearVal = {};
        clearVal.Format = m_desc.gbufferFormats[i];
        clearVal.Color[0] = 0.0f;
        clearVal.Color[1] = 0.0f;
        clearVal.Color[2] = 0.0f;
        clearVal.Color[3] = 0.0f;

        HRESULT hr =
            m_device->CreateCommittedResource(&defaultHeap, D3D12_HEAP_FLAG_NONE, &resDesc, D3D12_RESOURCE_STATE_COMMON,
                                              &clearVal, IID_PPV_ARGS(&m_gbuffer[i].resource));
        if (FAILED(hr)) {
            Logger::Logger::GetInstance()->Error(
                "[WindowFrameResources] G-buffer[{}] CreateCommittedResource failed hr=0x{:X}", i, hr);
            continue;
        }
        m_gbuffer[i].resource->SetName(kGBufNames[i]);

        // RTV
        m_gbuffer[i].rtvSlot = m_heaps->Allocate(m_heapTag, Resource::PartitionType::Rtv);
        if (m_gbuffer[i].rtvSlot == UINT32_MAX) {
            Logger::Logger::GetInstance()->Error("[WindowFrameResources] G-buffer[{}] RTV Allocate failed", i);
            continue;
        }
        m_gbuffer[i].rtvHandle =
            m_heaps->GetPartitionCpuHandle(Resource::PartitionType::Rtv, m_gbuffer[i].rtvSlot, m_heapTag);
        m_device->CreateRenderTargetView(m_gbuffer[i].resource.Get(), nullptr, m_gbuffer[i].rtvHandle);

        // SRV
        m_gbuffer[i].srvSlot = m_heaps->Allocate(m_heapTag, Resource::PartitionType::Texture);
        if (m_gbuffer[i].srvSlot == UINT32_MAX) {
            Logger::Logger::GetInstance()->Error("[WindowFrameResources] G-buffer[{}] SRV Allocate failed", i);
            continue;
        }
        D3D12_CPU_DESCRIPTOR_HANDLE srvCpu =
            m_heaps->GetPartitionCpuHandle(Resource::PartitionType::Texture, m_gbuffer[i].srvSlot, m_heapTag);
        D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
        srvDesc.Format = DXGI_FORMAT_UNKNOWN;
        srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srvDesc.Texture2D.MostDetailedMip = 0;
        srvDesc.Texture2D.MipLevels = 1;
        m_device->CreateShaderResourceView(m_gbuffer[i].resource.Get(), &srvDesc, srvCpu);
        m_gbuffer[i].srvCpuHandle = srvCpu;
        // GPU handle = CPU handle - heapStartCPU + heapStartGPU（同一描述符堆的 CPU/GPU 地址转换）
        auto *heap = m_heaps->GetHeap(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, m_heapTag);
        if (heap) {
            D3D12_CPU_DESCRIPTOR_HANDLE cpuStart = heap->GetCPUDescriptorHandleForHeapStart();
            D3D12_GPU_DESCRIPTOR_HANDLE gpuStart = heap->GetGPUDescriptorHandleForHeapStart();
            m_gbuffer[i].srvHandle.ptr = srvCpu.ptr - cpuStart.ptr + gpuStart.ptr;
        } else {
            m_gbuffer[i].srvHandle =
                m_heaps->GetPartitionGpuHandle(Resource::PartitionType::Texture, m_gbuffer[i].srvSlot, m_heapTag);
        }
    }

    // ── 2. SceneColor RT ──
    {
        D3D12_RESOURCE_DESC resDesc = {};
        resDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        resDesc.Width = width;
        resDesc.Height = height;
        resDesc.DepthOrArraySize = 1;
        resDesc.MipLevels = 1;
        resDesc.Format = m_desc.sceneColorFormat;
        resDesc.SampleDesc.Count = 1;
        resDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

        D3D12_CLEAR_VALUE clearVal = {};
        clearVal.Format = m_desc.sceneColorFormat;
        clearVal.Color[0] = 0.0f;
        clearVal.Color[1] = 0.0f;
        clearVal.Color[2] = 0.0f;
        clearVal.Color[3] = 0.0f;

        HRESULT hr =
            m_device->CreateCommittedResource(&defaultHeap, D3D12_HEAP_FLAG_NONE, &resDesc, D3D12_RESOURCE_STATE_COMMON,
                                              &clearVal, IID_PPV_ARGS(&m_sceneColor.resource));
        if (SUCCEEDED(hr)) {
            m_sceneColor.resource->SetName(L"WindowFrameResources_SceneColor");
        } else {
            Logger::Logger::GetInstance()->Error(
                "[WindowFrameResources] SceneColor CreateCommittedResource failed hr=0x{:X}", hr);
        }

        m_sceneColor.rtvSlot = m_heaps->Allocate(m_heapTag, Resource::PartitionType::Rtv);
        if (m_sceneColor.rtvSlot != UINT32_MAX) {
            m_sceneColor.rtvHandle =
                m_heaps->GetPartitionCpuHandle(Resource::PartitionType::Rtv, m_sceneColor.rtvSlot, m_heapTag);
            m_device->CreateRenderTargetView(m_sceneColor.resource.Get(), nullptr, m_sceneColor.rtvHandle);
        } else {
            Logger::Logger::GetInstance()->Error("[WindowFrameResources] SceneColor RTV Allocate failed");
        }

        m_sceneColor.srvSlot = m_heaps->Allocate(m_heapTag, Resource::PartitionType::Texture);
        if (m_sceneColor.srvSlot != UINT32_MAX) {
            D3D12_CPU_DESCRIPTOR_HANDLE srvCpu =
                m_heaps->GetPartitionCpuHandle(Resource::PartitionType::Texture, m_sceneColor.srvSlot, m_heapTag);
            D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
            srvDesc.Format = DXGI_FORMAT_UNKNOWN;
            srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            srvDesc.Texture2D.MostDetailedMip = 0;
            srvDesc.Texture2D.MipLevels = 1;
            m_device->CreateShaderResourceView(m_sceneColor.resource.Get(), &srvDesc, srvCpu);
            m_sceneColor.srvHandle =
                m_heaps->GetPartitionGpuHandle(Resource::PartitionType::Texture, m_sceneColor.srvSlot, m_heapTag);
        } else {
            Logger::Logger::GetInstance()->Error("[WindowFrameResources] SceneColor SRV Allocate failed");
        }
    }

    // ── 3. DepthStencil ──
    {
        D3D12_RESOURCE_DESC resDesc = {};
        resDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        resDesc.Width = width;
        resDesc.Height = height;
        resDesc.DepthOrArraySize = 1;
        resDesc.MipLevels = 1;
        resDesc.Format = m_desc.depthFormat;
        resDesc.SampleDesc.Count = 1;
        resDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

        D3D12_CLEAR_VALUE clearVal = {};
        clearVal.Format = m_desc.depthFormat;
        clearVal.DepthStencil.Depth = 1.0f;
        clearVal.DepthStencil.Stencil = 0;

        HRESULT hr =
            m_device->CreateCommittedResource(&defaultHeap, D3D12_HEAP_FLAG_NONE, &resDesc, D3D12_RESOURCE_STATE_COMMON,
                                              &clearVal, IID_PPV_ARGS(&m_depthStencil.resource));
        if (SUCCEEDED(hr)) {
            m_depthStencil.resource->SetName(L"WindowFrameResources_DepthStencil");
        } else {
            Logger::Logger::GetInstance()->Error(
                "[WindowFrameResources] DepthStencil CreateCommittedResource failed hr=0x{:X}", hr);
        }

        m_depthStencil.dsvSlot = m_heaps->Allocate(m_heapTag, Resource::PartitionType::Dsv);
        if (m_depthStencil.dsvSlot != UINT32_MAX) {
            m_depthStencil.dsvHandle =
                m_heaps->GetPartitionCpuHandle(Resource::PartitionType::Dsv, m_depthStencil.dsvSlot, m_heapTag);
            D3D12_DEPTH_STENCIL_VIEW_DESC dsvDesc = {};
            dsvDesc.Format = m_desc.depthFormat;
            dsvDesc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
            dsvDesc.Flags = D3D12_DSV_FLAG_NONE;
            m_device->CreateDepthStencilView(m_depthStencil.resource.Get(), &dsvDesc, m_depthStencil.dsvHandle);
        } else {
            Logger::Logger::GetInstance()->Error("[WindowFrameResources] DepthStencil DSV Allocate failed");
        }

        // Depth SRV（用于采样深度纹理）
        m_depthStencil.srvSlot = m_heaps->Allocate(m_heapTag, Resource::PartitionType::Texture);
        if (m_depthStencil.srvSlot != UINT32_MAX) {
            D3D12_CPU_DESCRIPTOR_HANDLE srvCpu =
                m_heaps->GetPartitionCpuHandle(Resource::PartitionType::Texture, m_depthStencil.srvSlot, m_heapTag);
            // 深度格式对应的 SRV 格式：D32_FLOAT → R32_FLOAT, D24_UNORM_S8_UINT → R24_UNORM_X8_TYPELESS
            DXGI_FORMAT srvFormat = DXGI_FORMAT_R32_FLOAT;
            if (m_desc.depthFormat == DXGI_FORMAT_D24_UNORM_S8_UINT)
                srvFormat = DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
            D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
            srvDesc.Format = srvFormat;
            srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            srvDesc.Texture2D.MostDetailedMip = 0;
            srvDesc.Texture2D.MipLevels = 1;
            m_device->CreateShaderResourceView(m_depthStencil.resource.Get(), &srvDesc, srvCpu);
            m_depthStencil.srvHandle =
                m_heaps->GetPartitionGpuHandle(Resource::PartitionType::Texture, m_depthStencil.srvSlot, m_heapTag);
        } else {
            Logger::Logger::GetInstance()->Error("[WindowFrameResources] DepthStencil SRV Allocate failed");
        }
    }

    m_width = width;
    m_height = height;

    Logger::Logger::GetInstance()->Info("[WindowFrameResources] AllocateResources complete: {}x{}", m_width, m_height);
}

void WindowFrameResources::FreeResources() {
    Logger::Logger::GetInstance()->Info("[WindowFrameResources] FreeResources");

    // ── G-buffer ──
    for (int i = 0; i < 4; ++i) {
        if (m_gbuffer[i].rtvSlot != UINT32_MAX && m_heaps) {
            m_heaps->Free(m_heapTag, Resource::PartitionType::Rtv, m_gbuffer[i].rtvSlot, 0);
            m_gbuffer[i].rtvSlot = UINT32_MAX;
        }
        m_gbuffer[i].rtvHandle = {};

        if (m_gbuffer[i].srvSlot != UINT32_MAX && m_heaps) {
            m_heaps->Free(m_heapTag, Resource::PartitionType::Texture, m_gbuffer[i].srvSlot, 0);
            m_gbuffer[i].srvSlot = UINT32_MAX;
        }
        m_gbuffer[i].srvHandle = {};

        m_gbuffer[i].resource.Reset();
    }

    // ── SceneColor ──
    if (m_sceneColor.rtvSlot != UINT32_MAX && m_heaps) {
        m_heaps->Free(m_heapTag, Resource::PartitionType::Rtv, m_sceneColor.rtvSlot, 0);
        m_sceneColor.rtvSlot = UINT32_MAX;
    }
    m_sceneColor.rtvHandle = {};

    if (m_sceneColor.srvSlot != UINT32_MAX && m_heaps) {
        m_heaps->Free(m_heapTag, Resource::PartitionType::Texture, m_sceneColor.srvSlot, 0);
        m_sceneColor.srvSlot = UINT32_MAX;
    }
    m_sceneColor.srvHandle = {};

    m_sceneColor.resource.Reset();

    // ── DepthStencil ──
    if (m_depthStencil.dsvSlot != UINT32_MAX && m_heaps) {
        m_heaps->Free(m_heapTag, Resource::PartitionType::Dsv, m_depthStencil.dsvSlot, 0);
        m_depthStencil.dsvSlot = UINT32_MAX;
    }
    m_depthStencil.dsvHandle = {};

    if (m_depthStencil.srvSlot != UINT32_MAX && m_heaps) {
        m_heaps->Free(m_heapTag, Resource::PartitionType::Texture, m_depthStencil.srvSlot, 0);
        m_depthStencil.srvSlot = UINT32_MAX;
    }
    m_depthStencil.srvHandle = {};

    m_depthStencil.resource.Reset();

    m_width = 0;
    m_height = 0;

    Logger::Logger::GetInstance()->Info("[WindowFrameResources] FreeResources complete");
}

// ========================================================================
// G-buffer 访问
// ========================================================================

ID3D12Resource *WindowFrameResources::GetGBufferResource(int i) const {
    if (i < 0 || i >= 4)
        return nullptr;
    return m_gbuffer[i].resource.Get();
}

D3D12_CPU_DESCRIPTOR_HANDLE WindowFrameResources::GetGBufferRTV(int i) const {
    if (i < 0 || i >= 4)
        return {};
    return m_gbuffer[i].rtvHandle;
}

D3D12_GPU_DESCRIPTOR_HANDLE WindowFrameResources::GetGBufferSRV(int i) const {
    if (i < 0 || i >= 4)
        return {};
    return m_gbuffer[i].srvHandle;
}

// ========================================================================
// SceneColor 访问
// ========================================================================

ID3D12Resource *WindowFrameResources::GetSceneColorResource() const { return m_sceneColor.resource.Get(); }

D3D12_CPU_DESCRIPTOR_HANDLE WindowFrameResources::GetSceneColorRTV() const { return m_sceneColor.rtvHandle; }

D3D12_GPU_DESCRIPTOR_HANDLE WindowFrameResources::GetSceneColorSRV() const { return m_sceneColor.srvHandle; }

// ========================================================================
// DepthStencil 访问
// ========================================================================

ID3D12Resource *WindowFrameResources::GetDepthResource() const { return m_depthStencil.resource.Get(); }

D3D12_CPU_DESCRIPTOR_HANDLE WindowFrameResources::GetDSV() const { return m_depthStencil.dsvHandle; }

D3D12_GPU_DESCRIPTOR_HANDLE WindowFrameResources::GetDepthSRV() const { return m_depthStencil.srvHandle; }
