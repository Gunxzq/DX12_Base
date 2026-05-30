#pragma once

#include "Common/d3dUtil.h"
#include "IRenderer.h"
#include "Renderer/RHI/Command/CommandList/CommandList.h"
#include "Resource/GpuResourceManager.h"
#include "Resource/Struct/GeometryHandle.h"
#include <wrl/client.h>

namespace DX12Engine::Resource {
class GeometryResourceManager;
class MaterialManager;
} // namespace DX12Engine::Resource

namespace DX12Engine::Renderer {

class D3D12DeviceContext;

class WaterRenderer : public IRenderer {
public:
    WaterRenderer() = default;
    ~WaterRenderer() = default;

    void SetDeviceContext(D3D12DeviceContext *context);
    void SetGeometryResourceManager(Resource::GeometryResourceManager *mgr) { m_geometryManager = mgr; }
    void SetMaterialManager(Resource::MaterialManager *mgr) { m_materialManager = mgr; }
    void Initialize();
    void OnResize(uint32_t width, uint32_t height);
    void Update(float deltaTime);
    void EndFrame();

    void BeginFrame(CommandList &cmdList, D3D12_GPU_VIRTUAL_ADDRESS passConstantsAddress,
                    D3D12_GPU_VIRTUAL_ADDRESS lightCBAddress, D3D12_GPU_DESCRIPTOR_HANDLE materialBufferSRV,
                    D3D12_GPU_VIRTUAL_ADDRESS waterCBAddress);

    void DrawWater(CommandList &cmdList, Resource::GeometryHandle geometryHandle, const DirectX::XMMATRIX &worldMatrix,
                   D3D12_GPU_VIRTUAL_ADDRESS objectCBAddress, D3D12_GPU_DESCRIPTOR_HANDLE textureSRV,
                   D3D12_GPU_DESCRIPTOR_HANDLE envMapSRV);

private:
    void CreateRootSignature();
    void CreatePSO();
    void LoadShaders();

    D3D12DeviceContext *m_context = nullptr;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> m_rootSignature;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_pso;
    Microsoft::WRL::ComPtr<ID3DBlob> m_vsBlob;
    Microsoft::WRL::ComPtr<ID3DBlob> m_psBlob;

    Resource::GeometryResourceManager *m_geometryManager = nullptr;
    Resource::MaterialManager *m_materialManager = nullptr;
};

} // namespace DX12Engine::Renderer