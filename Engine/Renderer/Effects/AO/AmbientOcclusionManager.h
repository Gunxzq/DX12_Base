#pragma once

#include <DirectXMath.h>
#include <cstdint>
#include <d3d12.h>

#include "Renderer/Pipeline/SsaoRenderer.h"
#include "Resource/Pool/DepthStencilPool.h"
#include "Resource/Pool/RenderTargetPool.h"
#include "Resource/Struct/DescriptorHandle.h"

namespace DX12Engine::Resource {
class DescriptorHeapCollection;
} // namespace DX12Engine::Resource

namespace DX12Engine::Renderer {

// 前向声明
class SsaoRenderer;

/**
 * @brief 屏幕空间 AO 算法枚举
 */
enum class AoAlgorithm : uint8_t {
    SSAO = 0,
    HBAO,
    // GTAO,  // 预留
    // VXAO,  // 预留
    Count
};

/**
 * @brief 环境光遮蔽管理器
 *
 * 持有 RTV 池资源（法线 RT + AO RT 双缓冲），统一管理分配和释放。
 * SsaoRenderer 不直接访问 RTV 池，通过本管理器获取 SRV/RTV handle。
 */
class AmbientOcclusionManager {
public:
    AmbientOcclusionManager() = default;
    ~AmbientOcclusionManager() = default;

    AmbientOcclusionManager(const AmbientOcclusionManager &) = delete;
    AmbientOcclusionManager &operator=(const AmbientOcclusionManager &) = delete;

    // ---- 生命周期 ----
    void SetDeviceContext(D3D12DeviceContext *context);
    void Initialize(ID3D12Device *device, Resource::DescriptorHeapCollection *descriptorHeaps, uint32_t renderWidth,
                    uint32_t renderHeight, Resource::HeapTag heapTag = Resource::HeapTag::Default);
    void Shutdown();

    // 随机向量纹理上传（命令管理器就绪后调用）
    void BuildRandomVectorTexture();

    // ---- 算法切换 ----
    void SetAlgorithm(AoAlgorithm algo);
    AoAlgorithm GetCurrentAlgorithm() const { return m_currentAlgorithm; }
    const char *GetAlgorithmName() const;
    uint32_t GetAlgorithmCount() const { return static_cast<uint32_t>(AoAlgorithm::Count); }

    // ---- PSO 注册 ----
    void SetAlgorithmPSO(AoAlgorithm algo, ID3D12PipelineState *pso);
    ID3D12PipelineState *GetCurrentPSO() const;

    // ---- AO 计算入口 ----
    void Execute(ID3D12GraphicsCommandList *cmdList, D3D12_GPU_DESCRIPTOR_HANDLE depthSRV,
                 D3D12_GPU_DESCRIPTOR_HANDLE normalSRV, const DirectX::XMFLOAT4X4 &view,
                 const DirectX::XMFLOAT4X4 &proj);

    // ---- 窗口缩放 ----
    void OnResize(uint32_t width, uint32_t height);

    // ---- 资源访问（给 SsaoRenderer 使用） ----
    D3D12_GPU_DESCRIPTOR_HANDLE GetAmbientMapSRV() const {
        if (!m_enabled)
            return m_fallbackWhiteSRV;
        return CpuSrvToGpu(m_ambientSRV);
    }
    D3D12_CPU_DESCRIPTOR_HANDLE GetAmbientMapRTV() const { return m_ambientRTV; }
    D3D12_GPU_DESCRIPTOR_HANDLE GetAmbientMap1SRV() const { return CpuSrvToGpu(m_ambient1SRV); }
    D3D12_CPU_DESCRIPTOR_HANDLE GetAmbientMap1RTV() const { return m_ambient1RTV; }

    // ---- 资源指针（屏障用） ----
    void SetEnabled(bool enabled) { m_enabled = enabled; }
    bool IsEnabled() const { return m_enabled; }
    void SetFallbackWhiteSRV(D3D12_GPU_DESCRIPTOR_HANDLE srv) { m_fallbackWhiteSRV = srv; }

    // ---- 资源屏障辅助（供 System 管理状态转换） ----
    ID3D12Resource *GetAmbientResource0() const;
    ID3D12Resource *GetAmbientResource1() const;

    // ---- 法线绘制 PSO/根签名（供 System 绘制场景几何体） ----
    // （已废弃：AO 从 G-buffer 读取法线，不再自行绘制）

    // ---- 静态烘培（仅编辑器模式） ----
    bool IsBakingEnabled() const { return m_bakingEnabled; }
    void SetBakingEnabled(bool enabled) { m_bakingEnabled = enabled; }
    void BakeStaticAO(uint32_t regionX, uint32_t regionY, uint32_t regionWidth, uint32_t regionHeight);

    // ---- 状态 ----
    bool IsInitialized() const { return m_initialized; }

    // ---- 单例 ----
    static AmbientOcclusionManager &GetInstance();

private:
    void BuildResources(uint32_t width, uint32_t height);
    void ReleaseResources();
    D3D12_GPU_DESCRIPTOR_HANDLE CpuSrvToGpu(D3D12_CPU_DESCRIPTOR_HANDLE cpuHandle) const;

private:
    ID3D12Device *m_device = nullptr;
    D3D12DeviceContext *m_deviceContext = nullptr;
    Resource::DescriptorHeapCollection *m_descriptorHeaps = nullptr;
    bool m_initialized = false;
    bool m_bakingEnabled = false;
    Resource::HeapTag m_heapTag = Resource::HeapTag::Default;

    AoAlgorithm m_currentAlgorithm = AoAlgorithm::SSAO;
    ID3D12PipelineState *m_algorithmPSOs[static_cast<uint32_t>(AoAlgorithm::Count)] = {};

    // RTV 池资源（主线程分配）
    Resource::RenderTargetHandle m_ambientRT0;
    Resource::RenderTargetHandle m_ambientRT1;

    // 当前分辨率（OnResize 防重复重建）
    uint32_t m_renderWidth = 0;
    uint32_t m_renderHeight = 0;

    // 缓存的 SRV/RTV handle
    D3D12_CPU_DESCRIPTOR_HANDLE m_ambientSRV = {};
    D3D12_CPU_DESCRIPTOR_HANDLE m_ambientRTV = {};
    D3D12_CPU_DESCRIPTOR_HANDLE m_ambient1SRV = {};
    D3D12_CPU_DESCRIPTOR_HANDLE m_ambient1RTV = {};

    // SSAO 渲染器（内部录制全屏四边形命令）
    SsaoRenderer m_ssaoRenderer;

    // 随机向量纹理资源（必须在管理器生命周期内保持存活）
    Microsoft::WRL::ComPtr<ID3D12Resource> m_randomVectorTexture;

    // SSAO 开关（关闭时返回材质系统的白色占位纹理）
    bool m_enabled = true;
    D3D12_GPU_DESCRIPTOR_HANDLE m_fallbackWhiteSRV = {};
};

} // namespace DX12Engine::Renderer
