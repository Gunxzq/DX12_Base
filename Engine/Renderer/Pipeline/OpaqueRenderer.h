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

    // ========================================================================
    // 渲染辅助接口（供游戏层 System 调用）
    // ========================================================================

    void SetGeometryResourceManager(Resource::GeometryResourceManager *mgr) { m_geometryManager = mgr; }
    void SetMaterialManager(Resource::MaterialManager *mgr) { m_materialManager = mgr; }

    void BeginFrame(CommandList &cmdList, D3D12_GPU_VIRTUAL_ADDRESS passConstantsAddress,
                    D3D12_GPU_VIRTUAL_ADDRESS lightCBAddress, D3D12_GPU_DESCRIPTOR_HANDLE materialBufferSRV,
                    D3D12_GPU_DESCRIPTOR_HANDLE shadowDataSRV, D3D12_GPU_DESCRIPTOR_HANDLE shadowMapSRV);

    // 单物体渲染（Standard 模式）
    void DrawMesh(CommandList &cmdList, DX12Engine::Resource::GeometryHandle geometryHandle,
                  const DirectX::XMMATRIX &worldMatrix, D3D12_GPU_VIRTUAL_ADDRESS objectCBAddress,
                  D3D12_GPU_DESCRIPTOR_HANDLE textureSRV, D3D12_GPU_DESCRIPTOR_HANDLE envMapSRV);

    // 实例化渲染（Instanced 模式）
    void DrawInstanced(CommandList &cmdList, DX12Engine::Resource::GeometryHandle geometryHandle,
                       D3D12_GPU_VIRTUAL_ADDRESS instanceBufferAddress, uint32_t instanceCount,
                       D3D12_GPU_DESCRIPTOR_HANDLE textureSRV, D3D12_GPU_DESCRIPTOR_HANDLE envMapSRV);

    void EndFrame();

private:
    // ========================================================================
    // 内部初始化
    // ========================================================================

    void CreateRootSignature();
    void CreatePSOs();
    void LoadShaders();

    // ========================================================================
    // 成员变量
    // ========================================================================

    D3D12DeviceContext *m_context = nullptr;

    // 根签名 & PSO
    Microsoft::WRL::ComPtr<ID3D12RootSignature> m_rootSignature; // 根签名（Standard 和 Instanced 共用）
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_psoStandard;   // 单物体模式
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_psoInstanced;  // 实例化模式

    Resource::GeometryResourceManager *m_geometryManager = nullptr;
    Resource::MaterialManager *m_materialManager = nullptr;

    // 着色器字节码
    Microsoft::WRL::ComPtr<ID3DBlob> m_vsStandard;  // 单物体 VS
    Microsoft::WRL::ComPtr<ID3DBlob> m_vsInstanced; // 实例化 VS
    Microsoft::WRL::ComPtr<ID3DBlob> m_psBlob;      // PS 共用

    // 投影矩阵（窗口 Resize 时更新）
    DirectX::XMMATRIX m_projectionMatrix;

    // 帧内 PSO 切换追踪（双队列方案下每帧仅切换 1 次）
    bool m_firstInstancedInFrame = true;
};

} // namespace DX12Engine::Renderer