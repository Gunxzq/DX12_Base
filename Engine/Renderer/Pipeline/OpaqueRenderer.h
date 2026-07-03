#pragma once

#include "Common/d3dUtil.h"
#include "ECS/Core/Registry.h"
#include "IRenderer.h"
#include "Renderer/RHI/Command/CommandList/CommandList.h"
#include "Resource/GpuResourceManager.h"
#include "Resource/Struct/GeometryHandle.h"
#include <array>
#include <memory>
#include <wrl/client.h>

namespace DX12Engine::ECS {
struct MeshComponent;
struct TransformComponent;
} // namespace DX12Engine::ECS

namespace DX12Engine::Resource {
class GeometryResourceManager;
class MaterialManager;
} // namespace DX12Engine::Resource

namespace DX12Engine::Renderer {

class D3D12DeviceContext;

class OpaqueRenderer : public IRenderer {
public:
    OpaqueRenderer() = default;
    ~OpaqueRenderer() = default;

    // ========================================================================
    // 生命周期管理
    // ========================================================================

    void SetDeviceContext(D3D12DeviceContext *context);
    void Initialize();
    void OnResize(uint32_t width, uint32_t height);
    void Update(float deltaTime);
    void EndFrame() override {}

    // ========================================================================
    // 依赖注入
    // ========================================================================

    void SetGeometryResourceManager(Resource::GeometryResourceManager *mgr) { m_geometryManager = mgr; }
    void SetMaterialManager(Resource::MaterialManager *mgr) { m_materialManager = mgr; }

    // ── G-buffer 输出（延迟渲染） ──
    void BeginFrameGBuffer(CommandList &cmdList, D3D12_GPU_VIRTUAL_ADDRESS passConstantsAddress,
                           D3D12_GPU_DESCRIPTOR_HANDLE materialBufferSRV,
                           D3D12_GPU_DESCRIPTOR_HANDLE textureHeapStart);
    void DrawInstancedGBuffer(CommandList &cmdList, DX12Engine::Resource::GeometryHandle geometryHandle,
                              D3D12_GPU_VIRTUAL_ADDRESS instanceBufferAddress, uint32_t instanceCount,
                              uint32_t startIndex = 0, int32_t startVertex = 0, uint32_t indexCount = 0);
    void EndFrameGBuffer();

private:
    // ========================================================================
    // 内部初始化
    // ========================================================================

    void LoadGBufferShader();
    void CreateGBufferRootSignature();
    void CreateGBufferPSO();

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
    Microsoft::WRL::ComPtr<ID3DBlob> m_vsBlob;         // 顶点着色器（G-buffer PSO 复用）
    Microsoft::WRL::ComPtr<ID3DBlob> m_psGBufferBlob;  // G-buffer 像素着色器

    // 投影矩阵（窗口 Resize 时更新）
    DirectX::XMMATRIX m_projectionMatrix;
};

} // namespace DX12Engine::Renderer
