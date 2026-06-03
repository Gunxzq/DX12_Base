#pragma once

#include "Common/d3dUtil.h"
#include "IRenderer.h"
#include "Renderer/RHI/Command/CommandList/CommandList.h"
#include "Renderer/Scene/LightManager/LightManager.h"
#include "Renderer/Scene/LightManager/LightResourceTypes.h"
#include "Resource/Struct/GeometryHandle.h"
#include <wrl/client.h>

namespace DX12Engine::Resource {
class DescriptorHeapCollection;
class GeometryResourceManager;
} // namespace DX12Engine::Resource

namespace DX12Engine::Renderer {

class D3D12DeviceContext;

// ============================================================================
// ShadowRenderer - 阴影贴图渲染器
//
// 职责：
//   - 为方向光/点光源/聚光灯渲染阴影贴图
//   - 使用 Shadow.hlsl 中的 VS/GS/PS
//   - 每个光源类型独立的 PSO
// ============================================================================
class ShadowRenderer : public IRenderer {
public:
    ShadowRenderer() = default;
    ~ShadowRenderer() = default;

    // ========================================================================
    // IRenderer 接口
    // ========================================================================
    void SetDeviceContext(D3D12DeviceContext *context) override;
    void Initialize() override;
    void OnResize(uint32_t width, uint32_t height) override;
    void Update(float deltaTime) override;
    void EndFrame() override;

    // ========================================================================
    // 依赖注入
    // ========================================================================
    void SetGeometryResourceManager(Resource::GeometryResourceManager *mgr) { m_geometryManager = mgr; }
    void SetDescriptorHeaps(Resource::DescriptorHeapCollection *heaps) { m_descriptorHeaps = heaps; }

    // ========================================================================
    // 阴影渲染
    // ========================================================================

    // 渲染方向光阴影贴图 (shadowCBAddress = GPU 地址，指向 DirLightShadowConstants)
    void RenderDirectionalShadow(CommandList &cmdList, D3D12_GPU_VIRTUAL_ADDRESS shadowCBAddress,
                                 const Renderer::DirShadowResources &shadowRes);

    // 渲染点光源阴影贴图（Cubemap 6面）
    void RenderPointShadow(CommandList &cmdList, D3D12_GPU_VIRTUAL_ADDRESS shadowCBAddress,
                           const Renderer::PointShadowResources &shadowRes);

    // 渲染聚光灯阴影贴图
    void RenderSpotShadow(CommandList &cmdList, D3D12_GPU_VIRTUAL_ADDRESS shadowCBAddress,
                          const Renderer::SpotShadowResources &shadowRes);

    // 绘制单个几何体的阴影（外部调用，遍历场景物体）
    void DrawShadowMesh(CommandList &cmdList, Resource::GeometryHandle geometryHandle,
                        const DirectX::XMMATRIX &worldMatrix, D3D12_GPU_VIRTUAL_ADDRESS objectCBAddress);

    // 结束阴影 Pass，将深度资源状态恢复为 SRV 供主 Pass 采样
    void EndShadowPass(CommandList &cmdList, Resource::GpuResourceHandle textureHandle);

private:
    // ========================================================================
    // 内部初始化
    // ========================================================================
    void LoadShaders();
    void CreateRootSignatures();
    void CreatePSOs();

    // ========================================================================
    // 成员变量
    // ========================================================================
    D3D12DeviceContext *m_context = nullptr;
    Resource::GeometryResourceManager *m_geometryManager = nullptr;
    Resource::DescriptorHeapCollection *m_descriptorHeaps = nullptr;

    // 根签名 (3 种光源类型共用，布局相同)
    Microsoft::WRL::ComPtr<ID3D12RootSignature> m_rootSignature;

    // PSO (方向光/点光源/聚光灯)
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_dirShadowPSO;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_pointShadowPSO;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_spotShadowPSO;

    // 着色器字节码
    Microsoft::WRL::ComPtr<ID3DBlob> m_dirVSBlob;
    Microsoft::WRL::ComPtr<ID3DBlob> m_pointVSBlob;
    Microsoft::WRL::ComPtr<ID3DBlob> m_pointGSBlob;
    Microsoft::WRL::ComPtr<ID3DBlob> m_spotVSBlob;
    Microsoft::WRL::ComPtr<ID3DBlob> m_psBlob;
    Microsoft::WRL::ComPtr<ID3DBlob> m_psPointBlob; // 点光源 PS (GeoOut 输入)
};

} // namespace DX12Engine::Renderer
