#pragma once

#include "Common/d3dUtil.h"
#include "IRenderer.h"
#include "Renderer/RHI/Command/CommandList/CommandList.h"
#include "Renderer/RenderItemBuilder/SkinnedRenderItem.h"
#include "Renderer/RenderItemBuilder/TRenderQueue.h"
#include "Resource/GpuResourceManager.h"
#include "Resource/Struct/GeometryHandle.h"
#include <wrl/client.h>

namespace DX12Engine::Resource {
class GeometryResourceManager;
class MaterialManager;
} // namespace DX12Engine::Resource

namespace DX12Engine::Renderer {

class D3D12DeviceContext;

class SkinnedRenderer : public IRenderer {
public:
    SkinnedRenderer() = default;
    ~SkinnedRenderer() = default;

    // ========================================================================
    // IRenderer 接口
    // ========================================================================
    void SetDeviceContext(D3D12DeviceContext *context) override;
    void Initialize() override;
    void OnResize(uint32_t width, uint32_t height) override;
    void Update(float deltaTime) override;
    void EndFrame() override {}

    // ========================================================================
    // 依赖注入
    // ========================================================================
    void SetGeometryResourceManager(Resource::GeometryResourceManager *mgr) { m_geometryManager = mgr; }
    void SetMaterialManager(Resource::MaterialManager *mgr) { m_materialManager = mgr; }

    // ========================================================================
    // G-buffer 蒙皮绘制（延迟渲染 Opaque phase）
    // ========================================================================
    void BeginFrameGBuffer(CommandList &cmdList, D3D12_GPU_VIRTUAL_ADDRESS passConstantsAddress,
                           D3D12_GPU_DESCRIPTOR_HANDLE materialBufferSRV,
                           D3D12_GPU_DESCRIPTOR_HANDLE textureHeapStart);
    void DrawGBuffer(CommandList &cmdList, const TRenderQueue<SkinnedRenderItem> &queue);
    void EndFrameGBuffer();

private:
    // ========================================================================
    // 内部初始化
    // ========================================================================
    void LoadGBufferShader();
    void CreateGBufferRootSignature();
    void CreateGBufferPSO();

    void DrawItems(CommandList &cmdList, const TRenderQueue<SkinnedRenderItem> &queue, ID3D12PipelineState *pso);

    // ========================================================================
    // 成员变量
    // ========================================================================
    D3D12DeviceContext *m_context = nullptr;

    // G-buffer 根签名 & PSO
    Microsoft::WRL::ComPtr<ID3D12RootSignature> m_gbufferRootSignature;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_gbufferPSO;

    Resource::GeometryResourceManager *m_geometryManager = nullptr;
    Resource::MaterialManager *m_materialManager = nullptr;

    // 着色器字节码
    Microsoft::WRL::ComPtr<ID3DBlob> m_vsBlob;         // 蒙皮顶点着色器（G-buffer PSO 复用）
    Microsoft::WRL::ComPtr<ID3DBlob> m_psGBufferBlob;  // G-buffer 像素着色器
};

} // namespace DX12Engine::Renderer
