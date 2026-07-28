#pragma once

#include "Common/d3dUtil.h"
#include "Resource/Core/DescriptorHeapCollection.h"
#include <wrl/client.h>

namespace DX12Engine::Resource {
class DescriptorHeapCollection;
} // namespace DX12Engine::Resource

namespace DX12Engine::Renderer {

// ========================================================================
// WindowFrameResources — 窗口帧资源管理器（引擎 CORE 层）
//
// 集中管理所有依赖窗口尺寸的 GPU 资源（G-buffer、SceneColor、DepthStencil 等），
// 统一处理 resize 时安全重建。
//
// 设计原则：
//   - 直接 CreateCommittedResource，不经过 RenderTargetPool
//   - resize 时阻塞式释放旧资源 + 创建新资源
//   - 描述符槽位通过 DescriptorHeapCollection 直接分配/释放
//   - 资源初始状态为 D3D12_RESOURCE_STATE_COMMON，各 System 使用前自行过渡
//
// 不管理的资源（尺寸无关，走各自管理器/池）：
//   - 阴影贴图（LightManager）
//   - 反射探针（ReflectionProbeManager）
//   - 后处理临时 RT（PostFx 管理器）
//   - 预览 RT（PreviewManager）
// ========================================================================
class WindowFrameResources {
public:
    /// 资源格式描述
    struct Desc {
        DXGI_FORMAT gbufferFormats[4] = {
            DXGI_FORMAT_R8G8B8A8_UNORM,     // albedo
            DXGI_FORMAT_R16G16B16A16_FLOAT, // normal
            DXGI_FORMAT_R8G8B8A8_UNORM,     // material
            DXGI_FORMAT_R16G16B16A16_FLOAT, // worldPos
        };
        DXGI_FORMAT sceneColorFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
        DXGI_FORMAT depthFormat = DXGI_FORMAT_D32_FLOAT;
    };

public:
    WindowFrameResources() = default;
    ~WindowFrameResources();

    WindowFrameResources(const WindowFrameResources &) = delete;
    WindowFrameResources &operator=(const WindowFrameResources &) = delete;

    /// 初始化：创建所有窗口尺寸资源
    /// @param device      D3D12 设备
    /// @param heaps       描述符堆集合
    /// @param width       窗口宽度
    /// @param height      窗口高度
    /// @param heapTag     描述符堆标签（Game=Default, Editor=EditorViewport）
    /// @param desc        资源格式描述（可选，默认使用 Desc 的缺省值）
    void Initialize(ID3D12Device *device, Resource::DescriptorHeapCollection *heaps, uint32_t width, uint32_t height,
                    Resource::HeapTag heapTag = Resource::HeapTag::Default, const Desc &desc = Desc{});

    /// Resize：安全重建所有资源
    /// 内部顺序：释放旧资源 → 分配描述符槽位 → 创建新资源
    void OnResize(uint32_t width, uint32_t height);

    /// 销毁所有资源
    void Shutdown();

    bool IsInitialized() const { return m_initialized; }

    // ── G-buffer ──

    int GetGBufferCount() const { return 4; }
    ID3D12Resource *GetGBufferResource(int i) const;
    D3D12_CPU_DESCRIPTOR_HANDLE GetGBufferRTV(int i) const;
    D3D12_GPU_DESCRIPTOR_HANDLE GetGBufferSRV(int i) const;

    // ── SceneColor ──

    ID3D12Resource *GetSceneColorResource() const;
    D3D12_CPU_DESCRIPTOR_HANDLE GetSceneColorRTV() const;
    D3D12_GPU_DESCRIPTOR_HANDLE GetSceneColorSRV() const;

    // ── DepthStencil ──

    ID3D12Resource *GetDepthResource() const;
    D3D12_CPU_DESCRIPTOR_HANDLE GetDSV() const;
    D3D12_GPU_DESCRIPTOR_HANDLE GetDepthSRV() const;

    // ── 尺寸 ──

    uint32_t GetWidth() const { return m_width; }
    uint32_t GetHeight() const { return m_height; }

private:
    void AllocateResources(uint32_t width, uint32_t height);
    void FreeResources();

    struct ResourceSlot {
        Microsoft::WRL::ComPtr<ID3D12Resource> resource;
        uint32_t rtvSlot = UINT32_MAX;
        uint32_t srvSlot = UINT32_MAX;
        D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = {};
        D3D12_CPU_DESCRIPTOR_HANDLE srvCpuHandle = {}; // SRV CPU handle（写入用）
        D3D12_GPU_DESCRIPTOR_HANDLE srvHandle = {};    // SRV GPU handle（shader 绑定用）
    };

    struct DepthSlot {
        Microsoft::WRL::ComPtr<ID3D12Resource> resource;
        uint32_t dsvSlot = UINT32_MAX;
        uint32_t srvSlot = UINT32_MAX;
        D3D12_CPU_DESCRIPTOR_HANDLE dsvHandle = {};
        D3D12_GPU_DESCRIPTOR_HANDLE srvHandle = {};
    };

    ResourceSlot m_gbuffer[4];
    ResourceSlot m_sceneColor;
    DepthSlot m_depthStencil;

    ID3D12Device *m_device = nullptr;
    Resource::DescriptorHeapCollection *m_heaps = nullptr;
    Resource::HeapTag m_heapTag = Resource::HeapTag::Default;
    Desc m_desc;
    uint32_t m_width = 0;
    uint32_t m_height = 0;
    bool m_initialized = false;
};

} // namespace DX12Engine::Renderer
