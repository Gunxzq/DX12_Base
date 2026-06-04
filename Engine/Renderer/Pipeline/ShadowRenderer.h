#pragma once

#include "Common/d3dUtil.h"
#include "IRenderer.h"
#include "Renderer/RHI/Command/CommandList/CommandList.h"
#include "Resource/Struct/GeometryHandle.h"
#include <wrl/client.h>

namespace DX12Engine::Resource {
class GeometryResourceManager;
} // namespace DX12Engine::Resource

namespace DX12Engine::Renderer {

class D3D12DeviceContext;

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

    // ========================================================================
    // 阴影 Pass 接口
    // ========================================================================

    void Begin(CommandList &cmdList, D3D12_GPU_VIRTUAL_ADDRESS lightCBAddress, D3D12_CPU_DESCRIPTOR_HANDLE dsvHandle,
               uint32_t width, uint32_t height);

    void DrawMesh(CommandList &cmdList, Resource::GeometryHandle geometryHandle, const DirectX::XMMATRIX &worldMatrix,
                  D3D12_GPU_VIRTUAL_ADDRESS objectCBAddress);

    // 结束阴影 Pass
    // @param cmdList 命令列表
    void End(CommandList &cmdList);

    // 获取 PSO（供外部调试）
    ID3D12PipelineState *GetPSO() const { return m_pso.Get(); }

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
    Resource::GeometryResourceManager *m_geometryManager = nullptr;

    // 根签名
    Microsoft::WRL::ComPtr<ID3D12RootSignature> m_rootSignature;

    // PSO
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_pso;

    // 着色器字节码
    Microsoft::WRL::ComPtr<ID3DBlob> m_vsBlob;
    Microsoft::WRL::ComPtr<ID3DBlob> m_psBlob;

    // 当前 Pass 状态
    bool m_inPass = false;
};

} // namespace DX12Engine::Renderer