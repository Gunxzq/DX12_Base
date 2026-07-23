#pragma once

#include "Boot/GameContext.h"
#include "Renderer/ApplicationRenderTargets.h"
#include "Resource/Pool/DepthStencilPool.h"
#include <memory>

namespace DX12Engine::ECS { class Registry; }
namespace DX12Engine::Renderer { class SkyRenderer; }
namespace DX12Engine::Scheduler { struct MessageContext; }

// ========================================================================
// EditorViewport — 编辑器视口预览渲染器
//
// 职责：
//   - 管理离屏 RT（通过 ApplicationRenderTargets）+ 深度缓冲 + 输出 SRV
//   - 注册 SkyboxRenderSystem + GridRenderSystem
// ========================================================================

class EditorViewport {
public:
    explicit EditorViewport(DX12Engine::Boot::GameContext *context);
    ~EditorViewport();

    EditorViewport(const EditorViewport &) = delete;
    EditorViewport &operator=(const EditorViewport &) = delete;

    bool Initialize();
    void Shutdown();

    void OnResize(uint32_t width, uint32_t height);

    /// 获取输出 SRV（供 EditorLayout::DrawViewport 使用）
    D3D12_GPU_DESCRIPTOR_HANDLE GetOutputSRV() const { return m_outputSRV; }

    /// 获取 ApplicationRenderTargets（供渲染管线系统使用）
    DX12Engine::Renderer::ApplicationRenderTargets *GetAppRTs() const { return m_appRTs.get(); }

    /// 获取深度缓冲 DSV 句柄（供渲染管线系统使用）
    D3D12_CPU_DESCRIPTOR_HANDLE GetDSVHandle() const { return m_dsvHandle; }

    /// 获取深度缓冲 Handle（供渲染管线系统做 ResourceBarrier 使用）
    DX12Engine::Resource::DepthStencilHandle GetDepthHandle() const { return m_depthHandle; }

    uint32_t GetWidth() const { return m_width; }
    uint32_t GetHeight() const { return m_height; }

private:
    bool CreateRenderTarget(uint32_t width, uint32_t height);
    void DestroyRenderTarget();
    bool CreateRenderResources();
    void DestroyRenderResources();
    void RegisterRenderSystems();

    DX12Engine::Boot::GameContext *m_context;

    // ── 离屏 RT（通过 ApplicationRenderTargets + DepthStencilPool 管理）──
    std::unique_ptr<DX12Engine::Renderer::ApplicationRenderTargets> m_appRTs;
    DX12Engine::Resource::DepthStencilHandle m_depthHandle;
    D3D12_CPU_DESCRIPTOR_HANDLE m_rtvHandle = {};
    D3D12_CPU_DESCRIPTOR_HANDLE m_dsvHandle = {};
    D3D12_CPU_DESCRIPTOR_HANDLE m_srvCpu = {};
    D3D12_GPU_DESCRIPTOR_HANDLE m_srvGpu = {};
    D3D12_GPU_DESCRIPTOR_HANDLE m_outputSRV = {};

    // ── 渲染器 ──
    // std::unique_ptr<DX12Engine::Renderer::GridRenderer> m_gridRenderer; // [已注释] 网格渲染待重构
    std::unique_ptr<DX12Engine::Renderer::SkyRenderer> m_skyRenderer;

    uint32_t m_width = 0;
    uint32_t m_height = 0;

    // ── 防抖 resize（等子窗口调整事件停止后再重建） ──
    uint32_t m_pendingWidth = 0;
    uint32_t m_pendingHeight = 0;

    bool m_initialized = false;
};