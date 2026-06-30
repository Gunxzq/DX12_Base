#pragma once

#include <DirectXMath.h>
#include <cstdint>
#include <d3d12.h>
#include <wrl/client.h>

#include "Resource/Struct/DescriptorHandle.h"

namespace DX12Engine::Renderer {

class D3D12DeviceContext;
class CommandList;

} // namespace DX12Engine::Renderer

namespace DX12Engine::Resource {
class DescriptorHeapCollection;
} // namespace DX12Engine::Resource

namespace DX12Engine::Renderer {

/**
 * @brief SSAO 渲染器
 *
 * 内部录制全屏四边形命令，不依赖 ECS 实体。
 * 法线 RT / AO RT 资源由 AmbientOcclusionManager 提供。
 */
class SsaoRenderer {
public:
    SsaoRenderer() = default;
    ~SsaoRenderer() = default;

    SsaoRenderer(const SsaoRenderer &) = delete;
    SsaoRenderer &operator=(const SsaoRenderer &) = delete;

    void SetDeviceContext(D3D12DeviceContext *context);
    void SetDescriptorHeaps(DX12Engine::Resource::DescriptorHeapCollection *heaps);
    void Initialize();
    void Shutdown();

    void Execute(CommandList &cmdList, ID3D12PipelineState *aoPSO, ID3D12PipelineState *blurPSO,
                 D3D12_GPU_DESCRIPTOR_HANDLE depthSRV, D3D12_GPU_DESCRIPTOR_HANDLE normalSRV,
                 D3D12_GPU_DESCRIPTOR_HANDLE ambientSRV, D3D12_CPU_DESCRIPTOR_HANDLE ambientRTV,
                 D3D12_GPU_DESCRIPTOR_HANDLE ambient1SRV, D3D12_CPU_DESCRIPTOR_HANDLE ambient1RTV,
                 ID3D12Resource *ambientRes0, ID3D12Resource *ambientRes1,
                 const DirectX::XMFLOAT4X4 &viewProj);

    // 公开 PSO 访问 + 随机纹理 SRV 注入
    ID3D12PipelineState *GetSSAOPipeline() const { return m_ssaoPSO.Get(); }
    ID3D12PipelineState *GetBlurPipeline() const { return m_blurPSO.Get(); }
    ID3D12PipelineState *GetNormalPipeline() const { return m_normalPSO.Get(); }
    ID3D12RootSignature *GetNormalRootSig() const { return m_normalRootSig.Get(); }
    void SetRandomVectorSRV(D3D12_GPU_DESCRIPTOR_HANDLE srv) { m_randomVectorMapSRV = srv; }

    void Resize(uint32_t width, uint32_t height);
    bool IsValid() const { return m_initialized; }

private:
    void BuildRootSignatures();
    void CreatePipelines();
    void ComputeAO(CommandList &cmdList, ID3D12PipelineState *aoPSO, D3D12_GPU_DESCRIPTOR_HANDLE depthSRV,
                   D3D12_GPU_DESCRIPTOR_HANDLE normalSRV);
    void DrawNormals(CommandList &cmdList, D3D12_CPU_DESCRIPTOR_HANDLE normalRTV, D3D12_CPU_DESCRIPTOR_HANDLE depthDSV);
    void BlurAO(CommandList &cmdList, ID3D12PipelineState *blurPSO, bool horizontal,
                D3D12_GPU_DESCRIPTOR_HANDLE srcSRV);

private:
    D3D12DeviceContext *m_deviceContext = nullptr;
    DX12Engine::Resource::DescriptorHeapCollection *m_descriptorHeaps = nullptr;
    bool m_initialized = false;
    uint32_t m_width = 0;
    uint32_t m_height = 0;

    // 内部资源
    D3D12_GPU_DESCRIPTOR_HANDLE m_randomVectorMapSRV = {};

    // 根签名
    Microsoft::WRL::ComPtr<ID3D12RootSignature> m_aoRootSig;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> m_blurRootSig;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> m_normalRootSig;

    // SSAO 常量缓冲
    Microsoft::WRL::ComPtr<ID3D12Resource> m_ssaoCB;
    void *m_ssaoCBMapped = nullptr;

    // SSAO PSO
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_ssaoPSO;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_blurPSO;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_normalPSO;
};

} // namespace DX12Engine::Renderer
