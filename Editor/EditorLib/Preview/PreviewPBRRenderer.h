#pragma once

#include "Common/d3dUtil.h"
#include "Renderer/Pipeline/IRenderer.h"
#include "Resource/Geometry/TriangleMesh.h"
#include <memory>
#include <wrl/client.h>

// 前向声明
namespace DX12Engine::Resource {
class GpuResourceManager;
class GeometryResourceManager;
} // namespace DX12Engine::Resource



namespace DX12Engine::Renderer { class D3D12DeviceContext; }
namespace DX12Engine::Renderer { class CommandList; }
namespace DX12Engine { class ErrorReporter; }

/**
 * @brief 预览 PBR 渲染器 — 管理预览 PSO 和内置球体 mesh
 *
 * 单 PSO 方案，CBV(b0) + SRV(t0) 根签名。
 * shader 通过 hasTexture 标志位切换纯色/纹理模式，
 * 材质数据中的纹理索引标识符控制回退行为。
 */
class PreviewPBRRenderer : public DX12Engine::Renderer::IRenderer {
public:
    PreviewPBRRenderer() = default;
    ~PreviewPBRRenderer() override = default;

    void SetDeviceContext(DX12Engine::Renderer::D3D12DeviceContext *context) override;
    void Initialize() override;
    void OnResize(uint32_t width, uint32_t height) override;
    void Update(float deltaTime) override;
    void EndFrame() override;

    /// 获取 PSO
    ID3D12PipelineState *GetPSO() const { return m_pso.Get(); }
    /// 获取 Unlit PSO（纹理预览用）
    ID3D12PipelineState *GetUnlitPSO() const { return m_psoUnlit.Get(); }
    ID3D12RootSignature *GetRootSignature() const { return m_rootSignature.Get(); }

    bool IsInitialized() const { return m_pso != nullptr && m_psoUnlit != nullptr; }

    /// 创建内置球体 mesh（用于纹理/材质预览）
    const DX12Engine::Resource::TriangleMesh *CreatePreviewSphere(
        DX12Engine::Resource::GpuResourceManager &gpuResMgr,
        DX12Engine::Resource::GeometryResourceManager *geoMgr);

    const DX12Engine::Resource::TriangleMesh *GetPreviewSphere() const { return m_previewSphere.get(); }

private:
    void LoadShaders();
    void CreateRootSignature();
    void CreatePSO();

    DX12Engine::Renderer::D3D12DeviceContext *m_context = nullptr;

    Microsoft::WRL::ComPtr<ID3D12RootSignature> m_rootSignature;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_pso;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_psoUnlit; // 纹理预览 Unlit PSO
    Microsoft::WRL::ComPtr<ID3DBlob> m_vs;
    Microsoft::WRL::ComPtr<ID3DBlob> m_ps;
    Microsoft::WRL::ComPtr<ID3DBlob> m_vsUnlit;
    Microsoft::WRL::ComPtr<ID3DBlob> m_psUnlit;

    std::unique_ptr<DX12Engine::Resource::TriangleMesh> m_previewSphere;
};

