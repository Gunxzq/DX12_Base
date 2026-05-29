#pragma once

#include "Common/d3dUtil.h"
#include "IRenderer.h"
#include "Renderer/RHI/Command/CommandList/CommandList.h"
#include "Resource/Struct/GeometryHandle.h"
#include <wrl/client.h>

namespace DX12Engine::Resource {
class GeometryResourceManager;
}

namespace DX12Engine::Renderer {

class D3D12DeviceContext;

class SkyRenderer : public IRenderer {
public:
    SkyRenderer() = default;
    ~SkyRenderer() = default;

    // ========================================================================
    // 生命周期管理
    // ========================================================================

    void SetDeviceContext(D3D12DeviceContext *context);
    void SetGeometryResourceManager(Resource::GeometryResourceManager *mgr) { m_geometryManager = mgr; }
    void Initialize();
    void OnResize(uint32_t width, uint32_t height);
    void Update(float deltaTime);

    // ========================================================================
    // 渲染辅助接口
    // ========================================================================

    void BeginFrame(CommandList &cmdList, D3D12_GPU_VIRTUAL_ADDRESS passConstantsAddress,
                    D3D12_GPU_DESCRIPTOR_HANDLE envMapSRV);

    void DrawSky(CommandList &cmdList, DX12Engine::Resource::GeometryHandle skyboxGeometry,
                 D3D12_GPU_VIRTUAL_ADDRESS objectCBAddress);

    void EndFrame();

private:
    // ========================================================================
    // 内部初始化
    // ========================================================================

    void CreateRootSignature();
    void CreatePSO();
    void LoadShaders();

    // ========================================================================
    // 成员变量
    // ========================================================================

    D3D12DeviceContext *m_context = nullptr;

    Microsoft::WRL::ComPtr<ID3D12RootSignature> m_rootSignature;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_pso;

    Resource::GeometryResourceManager *m_geometryManager = nullptr;

    Microsoft::WRL::ComPtr<ID3DBlob> m_vsBlob;
    Microsoft::WRL::ComPtr<ID3DBlob> m_psBlob;
};

} // namespace DX12Engine::Renderer