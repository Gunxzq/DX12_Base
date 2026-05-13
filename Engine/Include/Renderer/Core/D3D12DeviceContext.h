#pragma once

#include "Command/CommandManager.h"
#include "Common/Common.h"
#include "Common/d3dUtil.h"
#include "Renderer/Core/D3D12FeatureChecker.h"
#include "Renderer/Core/SwapChainManager.h"
#include <cstdint>
#include <memory>
#include <wrl/client.h>

// 前向声明
class GameTimer;

namespace DX12Engine {
namespace Renderer {

// ========================================================================
// D3D12DeviceContext - D3D12 核心上下文
// 负责初始化和管理 D3D12 的核心资源：
//   - ID3D12CommandQueue
//   - 命令列表和命令分配器
//   - Fence 同步
//
// 委托 SwapChainManager 管理：
//   - IDXGISwapChain
//   - 描述符堆 (RTV/DSV)
//   - 后台缓冲区和深度模板缓冲区
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
        int adapterIndex = -1;             // 适配器索引，-1 表示自动选择最佳适配器
        bool enableVsync = true;           // 是否启用垂直同步
        uint32_t swapChainBufferCount = 2; // 交换链缓冲区数量
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

    ID3D12Device *GetDevice() const { return md3dDevice; }
    ID3D12CommandQueue *GetCommandQueue() const;

    UINT GetCurrentBackBufferIndex() const { return m_swapChainManager.GetCurrentBackBufferIndex(); }
    ID3D12Resource *GetCurrentBackBuffer() const { return m_swapChainManager.GetCurrentBackBuffer(); }

    D3D12_CPU_DESCRIPTOR_HANDLE GetCurrentBackBufferView() const {
        return m_swapChainManager.GetCurrentBackBufferView();
    }
    D3D12_CPU_DESCRIPTOR_HANDLE GetDepthStencilView() const { return m_swapChainManager.GetDepthStencilView(); }

    UINT GetRtvDescriptorSize() const { return m_swapChainManager.GetRtvDescriptorSize(); }
    UINT GetDsvDescriptorSize() const { return m_swapChainManager.GetDsvDescriptorSize(); }
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

    // ── 命令管理器 ──

    /**
     * @brief 获取命令管理器
     * @return CommandManager& 命令管理器引用
     * @note 用于工作线程提交命令，主线程控制帧同步
     */
    CommandManager &GetCommandManager() { return m_commandManager; }
    const CommandManager &GetCommandManager() const { return m_commandManager; }

private:
    // ── 内部初始化 ──

    void UpdateViewportAndScissorRect();

    // ── 成员变量 ──

    InitParams mParams;

    // 功能检测器（内部管理设备和工厂）
    std::unique_ptr<D3D12FeatureChecker> m_featureChecker;

    // 设备指针由 D3D12FeatureChecker 管理，这里只持有裸指针
    ID3D12Device *md3dDevice = nullptr;

    // 交换链管理器
    SwapChainManager m_swapChainManager;

    D3D12_VIEWPORT mViewport;
    D3D12_RECT mScissorRect;

    UINT mCbvSrvUavDescriptorSize = 0;

    bool m4xMsaaState = false;
    UINT m4xMsaaQuality = 0;
    UINT mClientWidth = 0;
    UINT mClientHeight = 0;
    DXGI_FORMAT mBackBufferFormat = DXGI_FORMAT_UNKNOWN;
    DXGI_FORMAT mDepthStencilFormat = DXGI_FORMAT_UNKNOWN;

    // ── 命令管理器 ──
    CommandManager m_commandManager;
};

} // namespace Renderer
} // namespace DX12Engine