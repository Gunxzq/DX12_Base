#pragma once

#include <algorithm>
#include <cstdint>
#include <d3d12.h>
#include <vector>
#include <wrl/client.h>

#include "HzbRenderer.h"

namespace DX12Engine::Renderer {
class CommandList;
class CommandManager;
} // namespace DX12Engine::Renderer

namespace DX12Engine {
namespace Renderer {

class HzbManager {
public:
    HzbManager() = default;
    ~HzbManager() = default;

    HzbManager(const HzbManager &) = delete;
    HzbManager &operator=(const HzbManager &) = delete;

    void Initialize(ID3D12Device *device, Resource::DescriptorHeapCollection *descriptorHeaps, uint32_t renderWidth,
                    uint32_t renderHeight, Resource::HeapTag heapTag = Resource::HeapTag::Default,
                    CommandManager *cmdMgr = nullptr);
    void Shutdown();

    // ---- 窗口缩放（尺寸比对防重复重建，规则 9） ----
    void OnResize(uint32_t width, uint32_t height);

    // ---- HZB 构建入口（HZB_Build 阶段录制 CS 降采样命令） ----
    void Execute(CommandList &cmd, ID3D12Resource *depthRes, D3D12_GPU_DESCRIPTOR_HANDLE depthSRV);

    // ---- 资源访问（消费方：SSR/接触阴影/遮挡剔除） ----
    ID3D12Resource *GetHzbResource() const { return m_hzbTexture.Get(); }
    D3D12_GPU_DESCRIPTOR_HANDLE GetHzbSRV() const { return m_srvHandle; }
    uint32_t GetMipCount() const { return m_mipCount; }
    uint32_t GetWidth() const { return m_renderWidth; }
    uint32_t GetHeight() const { return m_renderHeight; }
    bool IsInitialized() const { return m_initialized; }

    // ---- 单例（对齐 AmbientOcclusionManager::GetInstance） ----
    static HzbManager &GetInstance();

private:
    void BuildResources(uint32_t width, uint32_t height);
    void ReleaseResources();
    /// 创建/重建后同步初始化 HZB 内容为远值 1.0（无效默认值纹理——首帧/场景切换兜底：
    /// HZB 未被构建时内容 = 1.0（远）→ objNear < 1.0 不剔 → 天然不误剔；对齐 BlankTextureProvider）
    void InitializeContentToFar();

    // mip 层级数：floor(log2(max(w,h))) + 1（到 1×1 为止，1280×720 = 11 级）
    static uint32_t ComputeMipCount(uint32_t width, uint32_t height) {
        const uint32_t maxDim = std::max(width, height);
        uint32_t mipCount = 1;
        while ((maxDim >> mipCount) > 0 && mipCount < 16)
            ++mipCount;
        return mipCount;
    }

private:
    ID3D12Device *m_device = nullptr;
    Resource::DescriptorHeapCollection *m_descriptorHeaps = nullptr;
    Resource::HeapTag m_heapTag = Resource::HeapTag::Default;
    Renderer::CommandManager *m_cmdMgr = nullptr; // 创建后同步初始化（远值 1.0 无效默认值纹理）
    bool m_initialized = false;

    // 当前分辨率（OnResize 防重复重建）
    uint32_t m_renderWidth = 0;
    uint32_t m_renderHeight = 0;
    uint32_t m_mipCount = 0;

    // HZB mip 链纹理（R32_FLOAT，ALLOW_UNORDERED_ACCESS，初始 COMMON——使用方对称屏障）
    Microsoft::WRL::ComPtr<ID3D12Resource> m_hzbTexture;

    // 描述符槽位
    uint32_t m_srvSlot = UINT32_MAX;     // 全链 SRV 槽位（消费方采样）
    uint32_t m_uavBaseSlot = UINT32_MAX; // mip UAV 连续槽位基址（[0..mipCount-1]，含 mip0=深度图拷贝）
    D3D12_GPU_DESCRIPTOR_HANDLE m_srvHandle = {};
    std::vector<D3D12_GPU_DESCRIPTOR_HANDLE> m_mipUAVs; // [0..mipCount-1]，mip0 = 深度图 1:1 拷贝

    // HZB 渲染器（内部录制 CS 降采样命令，对齐 AO 管理器持 SsaoRenderer）
    HzbRenderer m_renderer;
};

} // namespace Renderer
} // namespace DX12Engine
