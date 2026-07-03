#pragma once

#include "Common/d3dUtil.h"
#include "IRenderer.h"
#include "Renderer/RHI/Command/CommandList/CommandList.h"
#include <memory>
#include <wrl/client.h>

namespace DX12Engine::Renderer {

class D3D12DeviceContext;

// ========================================================================
// LightingRenderer — 延迟光照 Pass 全屏 Quad 渲染器
// 读取 G-buffer (Albedo/Normal/Material/WorldPos) + SSAO，输出到交换链
// ========================================================================
class LightingRenderer : public IRenderer {
public:
    LightingRenderer() = default;
    ~LightingRenderer() = default;

    // ========================================================================
    // 生命周期管理
    // ========================================================================

    void SetDeviceContext(D3D12DeviceContext *context);
    void Initialize();
    void OnResize(uint32_t width, uint32_t height);
    void Update(float deltaTime);

    // ========================================================================
    // 渲染接口
    // ========================================================================

    /// 开始光照 Pass：绑定根签名、常量缓冲、G-buffer SRV + SSAO
    void BeginFrame(CommandList &cmdList,
                    D3D12_GPU_VIRTUAL_ADDRESS passConstantsAddress,
                    D3D12_GPU_VIRTUAL_ADDRESS lightCBAddress,
                    D3D12_GPU_DESCRIPTOR_HANDLE albedoSrv,
                    D3D12_GPU_DESCRIPTOR_HANDLE normalSrv,
                    D3D12_GPU_DESCRIPTOR_HANDLE materialSrv,
                    D3D12_GPU_DESCRIPTOR_HANDLE worldPosSrv,
                    D3D12_GPU_DESCRIPTOR_HANDLE ssaoSrv,
                    D3D12_GPU_DESCRIPTOR_HANDLE envMapSrv = {},
                    D3D12_GPU_DESCRIPTOR_HANDLE cubemapArraySrv = {},
                    D3D12_GPU_DESCRIPTOR_HANDLE shadowDataSRV = {},
                    D3D12_GPU_DESCRIPTOR_HANDLE shadowMapSRV = {});

    /// 绘制全屏 Quad（4 顶点 TRIANGLESTRIP）
    void Draw(CommandList &cmdList);

    void EndFrame();

private:
    void CreateRootSignature();
    void CreatePSO();
    void LoadShaders();

    D3D12DeviceContext *m_context = nullptr;

    Microsoft::WRL::ComPtr<ID3D12RootSignature> m_rootSignature;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_pso;

    Microsoft::WRL::ComPtr<ID3DBlob> m_vsBlob;
    Microsoft::WRL::ComPtr<ID3DBlob> m_psBlob;
};

} // namespace DX12Engine::Renderer
