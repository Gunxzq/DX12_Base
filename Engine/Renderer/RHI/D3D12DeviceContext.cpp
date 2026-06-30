#include "Renderer/RHI/D3D12DeviceContext.h"
#include "Common/d3dx12.h"
#include "Resource/Core/DescriptorHeapCollection.h"

using namespace DX12Engine::Renderer;
using Microsoft::WRL::ComPtr;

D3D12DeviceContext::~D3D12DeviceContext() { Shutdown(); }

void D3D12DeviceContext::Shutdown() {
    // 先关闭命令管理器
    m_commandManager.Shutdown();

    // 关闭交换链管理器
    m_swapChainManager.Shutdown();

    // 注意：md3dDevice 由 m_featureChecker 管理，不要直接释放
    md3dDevice = nullptr;

    // 释放 FeatureChecker（会同时释放内部的 Device 和 Factory）
    m_featureChecker.reset();
}

bool D3D12DeviceContext::Initialize(const InitParams &params) {
    if (!params.hwnd) {
        ENGINE_ASSERT_MSG("D3D12DeviceContext: Window handle is required");
        return false;
    }

    mParams = params;
    mClientWidth = params.clientWidth;
    mClientHeight = params.clientHeight;
    mBackBufferFormat = params.backBufferFormat;
    mDepthStencilFormat = params.depthStencilFormat;
    m4xMsaaState = params.enable4xMsaa;

    // 创建并初始化功能检测器
    m_featureChecker = std::make_unique<D3D12FeatureChecker>();
    m_featureChecker->Initialize(params.enableDebugLayer, params.enableGPUBasedValidation);

    // 创建设备
    if (!m_featureChecker->CreateDevice(params.adapterIndex, params.minFeatureLevel)) {
        ENGINE_ASSERT_MSG("D3D12DeviceContext: Failed to create D3D12 device");
        return false;
    }

    // 获取设备指针
    md3dDevice = m_featureChecker->GetDevice();

    // 从 FeatureChecker 获取描述符大小信息
    const auto &deviceInfo = m_featureChecker->GetDeviceInfo();
    mCbvSrvUavDescriptorSize = deviceInfo.cbvSrvUavDescriptorSize;

    // 检查 MSAA 质量等级
    if (m4xMsaaState) {
        auto msaaSupport = m_featureChecker->CheckMsaaSupport(mBackBufferFormat, 4);
        if (msaaSupport.isSupported) {
            m4xMsaaQuality = msaaSupport.qualityLevels;
        } else {
            m4xMsaaState = false; // 如果不支持，禁用 MSAA
        }
    }

    // 初始化命令管理器（在所有基础资源创建后）
    m_commandManager.Initialize(md3dDevice, CommandManager::DEFAULT_FRAME_COUNT);

    // 初始化交换链管理器
    SwapChainManager::InitParams swapParams;
    swapParams.hwnd = params.hwnd;
    swapParams.width = params.clientWidth;
    swapParams.height = params.clientHeight;
    swapParams.format = params.backBufferFormat;
    swapParams.enableVsync = params.enableVsync;
    swapParams.bufferCount = params.swapChainBufferCount;
    swapParams.depthStencilFormat = params.depthStencilFormat;
    swapParams.enable4xMsaa = m4xMsaaState;
    swapParams.msaaQualityLevels = m4xMsaaQuality;

    m_swapChainManager.Initialize(md3dDevice, m_commandManager.GetGraphicsQueue()->Get(), swapParams);

    // 设置视口和裁剪矩形
    UpdateViewportAndScissorRect();
    OutputDebugStringW(L"[DEBUG] D3D12DeviceContext Initialized.\n");

    return true;
}

void D3D12DeviceContext::UpdateViewportAndScissorRect() {
    mViewport.TopLeftX = 0;
    mViewport.TopLeftY = 0;
    mViewport.Width = static_cast<float>(mClientWidth);
    mViewport.Height = static_cast<float>(mClientHeight);
    mViewport.MinDepth = 0.0f;
    mViewport.MaxDepth = 1.0f;

    mScissorRect = {0, 0, static_cast<LONG>(mClientWidth), static_cast<LONG>(mClientHeight)};
}

void D3D12DeviceContext::OnResize(uint32_t width, uint32_t height) {
    if (width == 0 || height == 0) {
        return;
    }
    OutputDebugStringW(L"[DEBUG] D3D12DeviceContext OnResize called.\n");

    mClientWidth = width;
    mClientHeight = height;

    m_commandManager.FlushAllQueues();

    // 委托给 SwapChainManager 处理调整大小
    m_swapChainManager.Resize(width, height);

    // 深度缓冲已重建，重新创建深度 SRV
    RebuildDepthSRV();

    UpdateViewportAndScissorRect();
}

ID3D12GraphicsCommandList *D3D12DeviceContext::BeginFrame() {

    m_commandManager.BeginFrame();

    return nullptr;
}

void D3D12DeviceContext::EndFrame() {

    m_commandManager.EndFrame();

    m_swapChainManager.Present(mParams.enableVsync);
}

void D3D12DeviceContext::FlushCommandQueue() { m_commandManager.Flush(D3D12_COMMAND_LIST_TYPE_DIRECT); }

void D3D12DeviceContext::FlushCommandQueue(D3D12_COMMAND_LIST_TYPE type) { m_commandManager.Flush(type); }

ID3D12CommandQueue *D3D12DeviceContext::GetCommandQueue() const {
    CommandQueue *queue = m_commandManager.GetGraphicsQueue();
    return queue ? queue->Get() : nullptr;
}

// ========================================================================
// 深度缓冲 SRV（初始化时显式创建，Resize 时重建；纯访问器 GetDepthSRV 为 const）
// ========================================================================

void D3D12DeviceContext::InitDepthSRV() {
    if (m_depthSrvSlot != UINT32_MAX || !md3dDevice || !m_descriptorHeaps) {
        return;
    }

    ID3D12Resource *depthBuffer = m_swapChainManager.GetDepthStencilBuffer();
    if (!depthBuffer) {
        return;
    }

    m_depthSrvSlot = m_descriptorHeaps->Allocate(Resource::PartitionType::Texture);
    if (m_depthSrvSlot == UINT32_MAX) {
        return;
    }

    D3D12_CPU_DESCRIPTOR_HANDLE cpuHandle =
        m_descriptorHeaps->GetPartitionCpuHandle(Resource::PartitionType::Texture, m_depthSrvSlot);
    m_depthSRV = m_descriptorHeaps->GetPartitionGpuHandle(Resource::PartitionType::Texture, m_depthSrvSlot);

    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    if (mDepthStencilFormat == DXGI_FORMAT_D32_FLOAT) {
        srvDesc.Format = DXGI_FORMAT_R32_FLOAT;
    } else {
        srvDesc.Format = DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
    }
    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.Texture2D.MipLevels = 1;
    srvDesc.Texture2D.MostDetailedMip = 0;

    md3dDevice->CreateShaderResourceView(depthBuffer, &srvDesc, cpuHandle);
}

void D3D12DeviceContext::RebuildDepthSRV() {
    if (m_depthSrvSlot != UINT32_MAX && m_descriptorHeaps) {
        m_descriptorHeaps->Free(Resource::PartitionType::Texture, m_depthSrvSlot, UINT64_MAX);
    }
    m_depthSrvSlot = UINT32_MAX;
    m_depthSRV = {};

    InitDepthSRV();
}