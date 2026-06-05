#pragma once

#include "Common/d3dUtil.h"
#include "IRenderer.h"
#include "Renderer/RHI/Command/CommandList/CommandList.h"
#include "Renderer/RenderItemBuilder/TerrainRenderItem.h"
#include "Renderer/Scene/TerrainManager/TerrainManager.h"
#include "Resource/Struct/GeometryHandle.h"
#include <wrl/client.h>

namespace DX12Engine::Resource {
class GeometryResourceManager;
class MaterialManager;
} // namespace DX12Engine::Resource

namespace DX12Engine::Renderer {

class D3D12DeviceContext;

// ============================================================================
// 地形渲染器 - 支持曲面细分（Tessellation）
// ============================================================================
class TerrainRenderer : public IRenderer {
public:
    TerrainRenderer() = default;
    ~TerrainRenderer() = default;

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
    void SetMaterialManager(Resource::MaterialManager *mgr) { m_materialManager = mgr; }

    // ========================================================================
    // 渲染接口
    // ========================================================================
    void BeginFrame(CommandList &cmdList, D3D12_GPU_VIRTUAL_ADDRESS passConstantsAddress,
                    D3D12_GPU_VIRTUAL_ADDRESS lightCBAddress, D3D12_GPU_DESCRIPTOR_HANDLE materialBufferSRV);

    void DrawTerrain(CommandList &cmdList, const TerrainRenderItem &item);

    // ========================================================================
    // PSO 切换
    // ========================================================================
    void SetStandardPSO(CommandList &cmdList) const;
    void SetInstancedPSO(CommandList &cmdList) const;

private:
    // ========================================================================
    // 内部初始化
    // ========================================================================
    void LoadShaders();
    void CreateRootSignature();
    void CreatePSOs();

    // ========================================================================
    // 辅助方法
    // ========================================================================
    void BindCommonResources(CommandList &cmdList, D3D12_GPU_VIRTUAL_ADDRESS passConstantsAddress,
                             D3D12_GPU_VIRTUAL_ADDRESS lightCBAddress, D3D12_GPU_DESCRIPTOR_HANDLE materialBufferSRV);

    // ========================================================================
    // 成员变量
    // ========================================================================
    D3D12DeviceContext *m_context = nullptr;
    Resource::GeometryResourceManager *m_geometryManager = nullptr;
    Resource::MaterialManager *m_materialManager = nullptr;

    // 根签名
    Microsoft::WRL::ComPtr<ID3D12RootSignature> m_rootSignature;

    // PSO（Standard / Instanced）
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_psoStandard;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_psoInstanced;

    // 着色器字节码
    Microsoft::WRL::ComPtr<ID3DBlob> m_vsStandard;  // 标准 VS（备用）
    Microsoft::WRL::ComPtr<ID3DBlob> m_vsInstanced; // 实例化 VS（SV_InstanceID）
    Microsoft::WRL::ComPtr<ID3DBlob> m_hs;          // Hull Shader
    Microsoft::WRL::ComPtr<ID3DBlob> m_ds;          // Domain Shader
    Microsoft::WRL::ComPtr<ID3DBlob> m_ps;          // Pixel Shader
};

} // namespace DX12Engine::Renderer