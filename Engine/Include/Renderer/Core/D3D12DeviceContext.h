#pragma once

#include "Common/Common.h"
#include "Common/d3dUtil.h"
#include <cstdint>
#include <wrl/client.h>

// 前向声明
class GameTimer;

namespace DX12Engine {
namespace Renderer {

// ========================================================================
// D3D12DeviceContext - D3D12 核心上下文
// 负责初始化和管理 D3D12 的核心资源：
//   - IDXGIFactory4
//   - ID3D12Device
//   - ID3D12CommandQueue
//   - IDXGISwapChain
//   - 描述符堆 (RTV/DSV)
//   - 命令列表和命令分配器
// ========================================================================

class D3D12DeviceContext {
public:
    // ── 配置参数 ──
    struct InitParams {
        HWND hwnd = nullptr;         // 窗口句柄（必须提供）
        uint32_t clientWidth = 1280; // 客户端宽度
        uint32_t clientHeight = 720; // 客户端高度
        DXGI_FORMAT backBufferFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
        DXGI_FORMAT depthStencilFormat = DXGI_FORMAT_D24_UNORM_S8_UINT;
        bool enableDebugLayer = false;                              // 是否启用调试层
        bool enable4xMsaa = false;                                  // 是否启用 4x MSAA
        D3D_FEATURE_LEVEL minFeatureLevel = D3D_FEATURE_LEVEL_11_0; // 最低功能级别
    };

public:
    D3D12DeviceContext() = default;
    ~D3D12DeviceContext();

    // 禁止拷贝和移动
    D3D12DeviceContext(const D3D12DeviceContext &) = delete;
    D3D12DeviceContext &operator=(const D3D12DeviceContext &) = delete;
    D3D12DeviceContext(D3D12DeviceContext &&) = delete;
    D3D12DeviceContext &operator=(D3D12DeviceContext &&) = delete;

    // ── 初始化 ──

    /**
     * @brief 初始化 D3D12 设备上下文
     * @param params 初始化参数
     * @return bool 是否成功
     */
    bool Initialize(const InitParams &params);

    /**
     * @brief 销毁并清理资源
     */
    void Shutdown();

    // ── 窗口消息处理 ──

    /**
     * @brief 处理窗口大小改变
     * @param width 新宽度
     * @param height 新高度
     */
    void OnResize(uint32_t width, uint32_t height);

    // ── 命令执行 ──

    /**
     * @brief 开始新的命令列表
     * @return ID3D12GraphicsCommandList* 命令列表指针
     */
    ID3D12GraphicsCommandList *BeginFrame();

    /**
     * @brief 提交并执行当前帧的命令列表
     */
    void EndFrame();

    /**
     * @brief 等待 GPU 完成所有命令
     */
    void FlushCommandQueue();

    // ── 描述符访问 ──

    ID3D12Device *GetDevice() const { return md3dDevice.Get(); }
    ID3D12CommandQueue *GetCommandQueue() const { return mCommandQueue.Get(); }
    IDXGISwapChain *GetSwapChain() const { return mSwapChain.Get(); }

    UINT GetCurrentBackBufferIndex() const { return mCurrBackBuffer; }
    ID3D12Resource *GetCurrentBackBuffer() const { return mSwapChainBuffer[mCurrBackBuffer].Get(); }

    D3D12_CPU_DESCRIPTOR_HANDLE GetCurrentBackBufferView() const;
    D3D12_CPU_DESCRIPTOR_HANDLE GetDepthStencilView() const;

    UINT GetRtvDescriptorSize() const { return mRtvDescriptorSize; }
    UINT GetDsvDescriptorSize() const { return mDsvDescriptorSize; }
    UINT GetCbvSrvUavDescriptorSize() const { return mCbvSrvUavDescriptorSize; }

    // ── 视口和裁剪矩形 ──

    const D3D12_VIEWPORT &GetViewport() const { return mViewport; }
    const D3D12_RECT &GetScissorRect() const { return mScissorRect; }

    // ── 格式查询 ──

    DXGI_FORMAT GetBackBufferFormat() const { return mBackBufferFormat; }
    DXGI_FORMAT GetDepthStencilFormat() const { return mDepthStencilFormat; }
    float GetAspectRatio() const { return static_cast<float>(mClientWidth) / static_cast<float>(mClientHeight); }

    // ── MSAA ──

    bool Is4xMsaaEnabled() const { return m4xMsaaState; }
    UINT Get4xMsaaQuality() const { return m4xMsaaQuality; }

private:
    // ── 内部初始化 ──

    void CreateFactory();
    void CreateDevice();
    void CreateCommandQueue();
    void CreateCommandAllocators();
    void CreateSwapChain();
    void CreateDescriptorHeaps();
    void CreateDepthStencilBuffer();
    void UpdateViewportAndScissorRect();

    // ── 辅助函数 ──

    void LogAdapters();
    void LogAdapterOutputs(IDXGIAdapter *adapter);
    void LogOutputDisplayModes(IDXGIOutput *output, DXGI_FORMAT format);

    // ── 成员变量 ──

    InitParams mParams;

    Microsoft::WRL::ComPtr<IDXGIFactory4> mFactory;
    Microsoft::WRL::ComPtr<ID3D12Device> md3dDevice;
    Microsoft::WRL::ComPtr<ID3D12Fence> mFence;

    Microsoft::WRL::ComPtr<ID3D12CommandQueue> mCommandQueue;
    Microsoft::WRL::ComPtr<ID3D12CommandAllocator> mDirectCmdListAlloc;
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> mCommandList;

    static constexpr int SwapChainBufferCount = 2;
    Microsoft::WRL::ComPtr<IDXGISwapChain> mSwapChain;
    Microsoft::WRL::ComPtr<ID3D12Resource> mSwapChainBuffer[SwapChainBufferCount];
    Microsoft::WRL::ComPtr<ID3D12Resource> mDepthStencilBuffer;

    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> mRtvHeap;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> mDsvHeap;

    D3D12_VIEWPORT mViewport;
    D3D12_RECT mScissorRect;

    UINT mRtvDescriptorSize = 0;
    UINT mDsvDescriptorSize = 0;
    UINT mCbvSrvUavDescriptorSize = 0;

    UINT64 mCurrentFence = 0;
    int mCurrBackBuffer = 0;

    bool m4xMsaaState = false;
    UINT m4xMsaaQuality = 0;
    UINT mClientWidth = 0;
    UINT mClientHeight = 0;
    DXGI_FORMAT mBackBufferFormat = DXGI_FORMAT_UNKNOWN;
    DXGI_FORMAT mDepthStencilFormat = DXGI_FORMAT_UNKNOWN;
};

} // namespace Renderer
} // namespace DX12Engine
