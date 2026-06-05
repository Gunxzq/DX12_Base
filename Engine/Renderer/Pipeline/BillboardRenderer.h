#pragma once

#include "Common/d3dUtil.h"
#include "IRenderer.h"
#include "Renderer/RHI/Command/CommandList/CommandList.h"
#include "Renderer/RenderItemBuilder/BillboardRenderItem.h"
#include <wrl/client.h>

namespace DX12Engine::Renderer {

class D3D12DeviceContext;

class BillboardRenderer : public IRenderer {
public:
    BillboardRenderer() = default;
    ~BillboardRenderer() = default;

    // ========================================================================
    // IRenderer 接口
    // ========================================================================
    void SetDeviceContext(D3D12DeviceContext *context) override;
    void Initialize() override;
    void OnResize(uint32_t width, uint32_t height) override;
    void Update(float deltaTime) override;
    void EndFrame() override;

    // ========================================================================
    // 渲染接口
    // ========================================================================
    void BeginFrame(CommandList &cmdList, D3D12_GPU_VIRTUAL_ADDRESS passConstantsAddress,
                    D3D12_GPU_VIRTUAL_ADDRESS lightCBAddress, D3D12_GPU_DESCRIPTOR_HANDLE materialBufferSRV,
                    D3D12_GPU_DESCRIPTOR_HANDLE textureHeapStart);

    void DrawBillboard(CommandList &cmdList, const BillboardRenderItem &item);

    // ========================================================================
    // PSO 切换
    // ========================================================================
    void SetPSO(CommandList &cmdList) const;

private:
    // ========================================================================
    // 内部初始化
    // ========================================================================
    void LoadShaders();
    void CreateRootSignature();
    void CreatePSO();

    // ========================================================================
    // 成员变量
    // ========================================================================
    D3D12DeviceContext *m_context = nullptr;

    // 根签名
    Microsoft::WRL::ComPtr<ID3D12RootSignature> m_rootSignature;

    // PSO
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_pso;

    // 着色器字节码
    Microsoft::WRL::ComPtr<ID3DBlob> m_vs; // 顶点着色器
    Microsoft::WRL::ComPtr<ID3DBlob> m_gs; // 几何着色器
    Microsoft::WRL::ComPtr<ID3DBlob> m_ps; // 像素着色器
};

} // namespace DX12Engine::Renderer