#pragma once

#include "Common/Common.h"
#include <d3d12.h>
#include <dxgi1_4.h>
#include <vector>
#include <wrl/client.h>

namespace DX12Engine::Renderer {

class SwapChainManager {
public:
    struct InitParams {
        HWND hwnd = nullptr;
        uint32_t width = 1280;
        uint32_t height = 720;
        DXGI_FORMAT format = DXGI_FORMAT_R8G8B8A8_UNORM;
        bool enableVsync = true;
        uint32_t bufferCount = 2;
        DXGI_FORMAT depthStencilFormat = DXGI_FORMAT_D24_UNORM_S8_UINT;
        bool enable4xMsaa = false;
        UINT msaaQualityLevels = 0;
    };

    SwapChainManager() = default;
    ~SwapChainManager();

    void Initialize(ID3D12Device *device, ID3D12CommandQueue *commandQueue, const InitParams &params);
    void Shutdown();

    // 窗口大小改变时调用
    void Resize(uint32_t width, uint32_t height);

    // 获取当前后台缓冲区资源
    ID3D12Resource *GetCurrentBackBuffer() const;

    ID3D12Resource *GetIndexBackBuffer(UINT index) const;

    // 获取当前后台缓冲区的索引
    UINT GetCurrentIndex() const { return m_currBackBuffer; }

    // 呈现下一帧
    void Present(bool vsync = true);

    void CreateRenderTargetViews(ID3D12Device *device);
    void CreateDepthStencilView(ID3D12Device *device);

    void ReleaseRenderTargetViews();
    void ReleaseDepthStencilView();

    D3D12_CPU_DESCRIPTOR_HANDLE GetCurrentBackBufferView() const;
    D3D12_CPU_DESCRIPTOR_HANDLE GetDepthStencilView() const;

    // 获取描述符大小
    UINT GetRtvDescriptorSize() const { return m_rtvDescriptorSize; }
    UINT GetDsvDescriptorSize() const { return m_dsvDescriptorSize; }

private:
    Microsoft::WRL::ComPtr<IDXGISwapChain3> m_swapChain;
    std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>> m_backBuffers;

    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> m_rtvHeap;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_depthStencilBuffer;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> m_dsvHeap;

    InitParams m_params;
    UINT m_currBackBuffer = 0;
    UINT m_rtvDescriptorSize = 0;
    UINT m_dsvDescriptorSize = 0;

    bool m_enable4xMsaa = false;
    UINT m_4xMsaaQuality = 0;

    ID3D12Device *m_device = nullptr;
};

} // namespace DX12Engine::Renderer