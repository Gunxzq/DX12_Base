#include "Renderer/Core/SwapChainManager.h"
#include "Common/Common.h"
#include "Common/ThrowHelper.h"
#include "Common/d3dUtil.h"

namespace DX12Engine::Renderer {

SwapChainManager::~SwapChainManager() { Shutdown(); }

void SwapChainManager::Initialize(ID3D12Device *device, ID3D12CommandQueue *commandQueue, const InitParams &params) {
    m_params = params;
    m_device = device;

    DXGI_SWAP_CHAIN_DESC1 sd = {};
    sd.Width = params.width;
    sd.Height = params.height;
    sd.Format = params.format;

    sd.SampleDesc.Count = params.enable4xMsaa ? 4 : 1;
    sd.SampleDesc.Quality = params.enable4xMsaa ? (params.msaaQualityLevels - 1) : 0;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount = params.bufferCount;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    sd.AlphaMode = DXGI_ALPHA_MODE_UNSPECIFIED;
    sd.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;

    Microsoft::WRL::ComPtr<IDXGIFactory4> factory;
    ThrowIfFailed(CreateDXGIFactory1(IID_PPV_ARGS(&factory)));

    Microsoft::WRL::ComPtr<IDXGISwapChain1> swapChain1;
    ThrowIfFailed(factory->CreateSwapChainForHwnd(commandQueue, params.hwnd, &sd, nullptr, nullptr, &swapChain1));
    ThrowIfFailed(swapChain1.As(&m_swapChain));

    // 禁用 Alt+Enter 全屏切换，由我们手动控制
    factory->MakeWindowAssociation(params.hwnd, DXGI_MWA_NO_ALT_ENTER);

    m_backBuffers.resize(params.bufferCount);
    for (UINT i = 0; i < params.bufferCount; ++i) {
        ThrowIfFailed(m_swapChain->GetBuffer(i, IID_PPV_ARGS(&m_backBuffers[i])));
    }

    m_currBackBuffer = m_swapChain->GetCurrentBackBufferIndex();

    // 获取描述符大小
    m_rtvDescriptorSize = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    m_dsvDescriptorSize = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);

    // 创建 RTV 和 DSV
    CreateRenderTargetViews(device);
    CreateDepthStencilView(device);

    OutputDebugStringW(L"[DEBUG] SwapChainManager Initialized. Initial BackBuffer Index: ");
    OutputDebugStringW(std::to_wstring(m_currBackBuffer).c_str());
    OutputDebugStringW(L"\n");
}

void SwapChainManager::Shutdown() {
    ReleaseRenderTargetViews();
    ReleaseDepthStencilView();
    m_backBuffers.clear();
    m_swapChain.Reset();
}

void SwapChainManager::Resize(uint32_t width, uint32_t height) {
    if (width == 0 || height == 0)
        return;

    OutputDebugStringW(L"[DEBUG] SwapChainManager Resizing...\n");

    // 释放现有的 RTV 和 DSV
    ReleaseRenderTargetViews();
    ReleaseDepthStencilView();

    m_backBuffers.clear();

    ThrowIfFailed(m_swapChain->ResizeBuffers(m_params.bufferCount, width, height, m_params.format,
                                             DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH));

    m_params.width = width;
    m_params.height = height;

    m_backBuffers.resize(m_params.bufferCount);
    for (UINT i = 0; i < m_params.bufferCount; ++i) {
        ThrowIfFailed(m_swapChain->GetBuffer(i, IID_PPV_ARGS(&m_backBuffers[i])));
    }

    m_currBackBuffer = m_swapChain->GetCurrentBackBufferIndex();

    // 重建 RTV 和 DSV
    CreateRenderTargetViews(m_device);
    CreateDepthStencilView(m_device);

    OutputDebugStringW(L"[DEBUG] SwapChainManager Resize Complete. New BackBuffer Index: ");
    OutputDebugStringW(std::to_wstring(m_currBackBuffer).c_str());
    OutputDebugStringW(L"\n");
}

void SwapChainManager::CreateRenderTargetViews(ID3D12Device *device) {
    // 创建 RTV 堆
    D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc = {};
    rtvHeapDesc.NumDescriptors = static_cast<UINT>(m_backBuffers.size());
    rtvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    rtvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    rtvHeapDesc.NodeMask = 0;
    ThrowIfFailed(device->CreateDescriptorHeap(&rtvHeapDesc, IID_PPV_ARGS(&m_rtvHeap)));

    // 为每个缓冲区创建 RTV
    CD3DX12_CPU_DESCRIPTOR_HANDLE rtvHeapHandle(m_rtvHeap->GetCPUDescriptorHandleForHeapStart());
    for (UINT i = 0; i < m_backBuffers.size(); i++) {
        device->CreateRenderTargetView(m_backBuffers[i].Get(), nullptr, rtvHeapHandle);
        rtvHeapHandle.Offset(1, m_rtvDescriptorSize);
    }
}

void SwapChainManager::CreateDepthStencilView(ID3D12Device *device) {
    // 创建 DSV 堆
    D3D12_DESCRIPTOR_HEAP_DESC dsvHeapDesc = {};
    dsvHeapDesc.NumDescriptors = 1;
    dsvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
    dsvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    dsvHeapDesc.NodeMask = 0;
    ThrowIfFailed(device->CreateDescriptorHeap(&dsvHeapDesc, IID_PPV_ARGS(&m_dsvHeap)));

    // 创建深度模板缓冲区
    D3D12_RESOURCE_DESC depthStencilDesc = {};
    depthStencilDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    depthStencilDesc.Alignment = 0;
    depthStencilDesc.Width = m_params.width;
    depthStencilDesc.Height = m_params.height;
    depthStencilDesc.DepthOrArraySize = 1;
    depthStencilDesc.MipLevels = 1;
    depthStencilDesc.Format = DXGI_FORMAT_R24G8_TYPELESS; // Typeless for both SRV and DSV
    depthStencilDesc.SampleDesc.Count = m_enable4xMsaa ? 4 : 1;
    depthStencilDesc.SampleDesc.Quality = m_enable4xMsaa ? (m_4xMsaaQuality - 1) : 0;
    depthStencilDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    depthStencilDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

    D3D12_CLEAR_VALUE optClear = {};
    optClear.Format = m_params.depthStencilFormat;
    optClear.DepthStencil.Depth = 1.0f;
    optClear.DepthStencil.Stencil = 0;

    CD3DX12_HEAP_PROPERTIES heapProps(D3D12_HEAP_TYPE_DEFAULT);
    ThrowIfFailed(device->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &depthStencilDesc,
                                                  D3D12_RESOURCE_STATE_DEPTH_WRITE, &optClear,
                                                  IID_PPV_ARGS(&m_depthStencilBuffer)));

    // Create DSV
    D3D12_DEPTH_STENCIL_VIEW_DESC dsvDesc = {};
    dsvDesc.Flags = D3D12_DSV_FLAG_NONE;
    dsvDesc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
    dsvDesc.Format = m_params.depthStencilFormat;
    dsvDesc.Texture2D.MipSlice = 0;
    device->CreateDepthStencilView(m_depthStencilBuffer.Get(), &dsvDesc, GetDepthStencilView());
}

void SwapChainManager::ReleaseRenderTargetViews() { m_rtvHeap.Reset(); }

void SwapChainManager::ReleaseDepthStencilView() {
    m_depthStencilBuffer.Reset();
    m_dsvHeap.Reset();
}

D3D12_CPU_DESCRIPTOR_HANDLE SwapChainManager::GetCurrentBackBufferView() const {
    return CD3DX12_CPU_DESCRIPTOR_HANDLE(m_rtvHeap->GetCPUDescriptorHandleForHeapStart(), m_currBackBuffer,
                                         m_rtvDescriptorSize);
}

D3D12_CPU_DESCRIPTOR_HANDLE SwapChainManager::GetDepthStencilView() const {
    return m_dsvHeap->GetCPUDescriptorHandleForHeapStart();
}

ID3D12Resource *SwapChainManager::GetCurrentBackBuffer() const {

    OutputDebugStringW(L"[DEBUG] GetCurrentBackBuffer called. Index: ");
    OutputDebugStringW(std::to_wstring(m_currBackBuffer).c_str());
    OutputDebugStringW(L"\n");
    return m_backBuffers[m_currBackBuffer].Get();
}

ID3D12Resource *SwapChainManager::GetBackBuffer(UINT index) const {
    if (index >= m_backBuffers.size()) {
        return nullptr;
    }
    return m_backBuffers[index].Get();
}

void SwapChainManager::Present(bool vsync) {
    OutputDebugStringW(L"[DEBUG] Presenting. Current Index before Present: ");
    OutputDebugStringW(std::to_wstring(m_currBackBuffer).c_str());
    OutputDebugStringW(L"\n");

    m_swapChain->Present(vsync ? 1 : 0, 0);
    m_currBackBuffer = (m_currBackBuffer + 1) % m_params.bufferCount;

    OutputDebugStringW(L"[DEBUG] Present complete. New Index: ");
    OutputDebugStringW(std::to_wstring(m_currBackBuffer).c_str());
    OutputDebugStringW(L"\n");
}

} // namespace DX12Engine::Renderer