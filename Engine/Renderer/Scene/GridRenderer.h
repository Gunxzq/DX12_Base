#pragma once

#include "Common/d3dUtil.h"
#include "Renderer/Pipeline/IRenderer.h"
#include <d3d12.h>
#include <wrl/client.h>

namespace DX12Engine::Renderer {

class D3D12DeviceContext;

// ========================================================================
// GridRenderer — 3D 无限网格渲染器
//
// 方案：大 Quad + 像素着色器计算网格线
//   - 根签名：1 个 CBV（b0: GridCB）
//   - 几何体：大 Quad（2 个三角形），VS 中缩放偏移到相机位置
//   - PS：多级 LOD + 反走样 + 距离衰减 + 三轴高亮
// ========================================================================

class GridRenderer : public IRenderer {
public:
    GridRenderer() = default;
    ~GridRenderer() override { Shutdown(); }

    GridRenderer(const GridRenderer &) = delete;
    GridRenderer &operator=(const GridRenderer &) = delete;

    // ========================================================================
    // IRenderer 接口
    // ========================================================================
    void SetDeviceContext(D3D12DeviceContext *context) override;
    void Initialize() override;
    void OnResize(uint32_t width, uint32_t height) override;
    void EndFrame() override;
    void Update(float deltaTime) override;

    bool IsInitialized() const { return m_initialized; }

    /// 绘制网格到当前绑定的 RT
    /// @param cmdList 命令列表
    /// @param viewProj 相机 ViewProj 矩阵
    /// @param cameraPos 相机世界位置
    void Draw(ID3D12GraphicsCommandList *cmdList, const DirectX::XMMATRIX &viewProj,
              const DirectX::XMFLOAT3 &cameraPos);

private:
    void CreateRootSignature();
    void CreatePSO(ID3D12Device *device, DXGI_FORMAT depthFormat);
    void Shutdown();

    D3D12DeviceContext *m_context = nullptr;

    Microsoft::WRL::ComPtr<ID3DBlob> m_vsBlob;
    Microsoft::WRL::ComPtr<ID3DBlob> m_psBlob;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> m_rootSig;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_pso;
    bool m_initialized = false;
};

} // namespace DX12Engine::Renderer