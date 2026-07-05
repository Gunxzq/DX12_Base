#include "OpaqueRenderer.h"
#include "Common/d3dUtil.h"
#include "ECS/Core/Components.h"
#include "Renderer/RHI/D3D12DeviceContext.h"
#include "Renderer/Utils/GeometryGenerator.h"
#include "Resource/Geometry/TriangleMesh.h"
#include "Resource/GpuResourceManager.h"
#include "Resource/Manager/GeometryResourceManager.h"
#include <DirectXMath.h>
#include <d3dcompiler.h>
#include <entt/entt.hpp>

using namespace DirectX;
using namespace DX12Engine::ECS;
using namespace DX12Engine::Renderer;
using namespace DX12Engine::Resource;

namespace DX12Engine::Renderer {

// ========================================================================
// 生命周期管理
// ========================================================================

void OpaqueRenderer::SetDeviceContext(D3D12DeviceContext *context) { m_context = context; }

void OpaqueRenderer::Initialize() {
    if (!m_context) {
        throw std::runtime_error("OpaqueRenderer: Device context not set before Initialize");
    }

    // 编译顶点着色器（G-buffer PSO 共用）
    {
        UINT compileFlags = D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION | D3DCOMPILE_ENABLE_UNBOUNDED_DESCRIPTOR_TABLES;
        Microsoft::WRL::ComPtr<ID3DBlob> errors = nullptr;
        HRESULT hr = D3DCompileFromFile(L"Shaders/color.hlsl", nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE, "VS", "vs_5_1",
                                        compileFlags, 0, &m_vsBlob, &errors);
        if (FAILED(hr)) {
            if (errors) {
                const char *errMsg = reinterpret_cast<const char *>(errors->GetBufferPointer());
                OutputDebugStringA("=== VS COMPILATION ERROR ===\n");
                OutputDebugStringA(errMsg);
                OutputDebugStringA("============================\n");
            }
            throw std::runtime_error("OpaqueRenderer: Failed to compile Vertex Shader");
        }
    }

    LoadGBufferShader();

    if (!m_vsBlob || !m_psGBufferBlob) {
        ErrorReporter::Fatal("OpaqueRenderer: Failed to load shaders");
    }

    CreateGBufferRootSignature();
    CreateGBufferPSO();

    const auto &viewport = m_context->GetViewport();
    OnResize(static_cast<uint32_t>(viewport.Width), static_cast<uint32_t>(viewport.Height));

    OutputDebugStringW(L"[INFO] OpaqueRenderer initialized (GBuffer only)\n");
}

void OpaqueRenderer::OnResize(uint32_t width, uint32_t height) {
    if (width == 0 || height == 0) {
        return;
    }

    float aspectRatio = static_cast<float>(width) / static_cast<float>(height);
    m_projectionMatrix = XMMatrixPerspectiveFovLH(XM_PIDIV4, aspectRatio, 1.0f, 1000.0f);
}

void OpaqueRenderer::Update(float deltaTime) {
    // 当前示例中不需要每帧更新逻辑
}

// ========================================================================
// G-buffer 渲染接口
// ========================================================================

void OpaqueRenderer::BeginFrameGBuffer(CommandList &cmdList, D3D12_GPU_VIRTUAL_ADDRESS passConstantsAddress,
                                       D3D12_GPU_DESCRIPTOR_HANDLE materialBufferSRV,
                                       D3D12_GPU_DESCRIPTOR_HANDLE textureHeapStart) {
    if (!m_gbufferPSO || !m_gbufferRootSignature)
        return;

    cmdList.Get()->SetPipelineState(m_gbufferPSO.Get());
    cmdList.Get()->SetGraphicsRootSignature(m_gbufferRootSignature.Get());
    cmdList.Get()->SetGraphicsRootConstantBufferView(0, passConstantsAddress);

    if (materialBufferSRV.ptr != 0)
        cmdList.Get()->SetGraphicsRootDescriptorTable(1, materialBufferSRV);
    if (textureHeapStart.ptr != 0)
        cmdList.Get()->SetGraphicsRootDescriptorTable(2, textureHeapStart);
}

void OpaqueRenderer::DrawInstancedGBuffer(CommandList &cmdList, GeometryHandle geometryHandle,
                                          D3D12_GPU_VIRTUAL_ADDRESS instanceBufferAddress, uint32_t instanceCount,
                                          uint32_t startIndex, int32_t startVertex, uint32_t indexCount) {
    if (!m_geometryManager) return;

    const TriangleMesh *mesh = m_geometryManager->GetGeometry<TriangleMesh>(geometryHandle);
    if (!mesh || !mesh->isGpuReady) return;

    auto &gpuMgr = GpuResourceManager::GetInstance();
    ID3D12Resource *vb = gpuMgr.GetResource(mesh->vertexBufferHandle);
    ID3D12Resource *ib = gpuMgr.GetResource(mesh->indexBufferHandle);
    if (!vb || !ib) return;

    D3D12_VERTEX_BUFFER_VIEW vbView = {vb->GetGPUVirtualAddress(),
                                       (UINT)(mesh->vertexCount * mesh->vertexStride), mesh->vertexStride};
    D3D12_INDEX_BUFFER_VIEW ibView = {ib->GetGPUVirtualAddress(),
                                      (UINT)(mesh->indexCount * (mesh->indexFormat == DXGI_FORMAT_R32_UINT ? 4 : 2)),
                                      mesh->indexFormat};
    cmdList.Get()->IASetVertexBuffers(0, 1, &vbView);
    cmdList.Get()->IASetIndexBuffer(&ibView);
    cmdList.Get()->IASetPrimitiveTopology(mesh->topology);

    // slot 3: t12,space1 InstanceData
    cmdList.Get()->SetGraphicsRootShaderResourceView(3, instanceBufferAddress);

    uint32_t actualIndexCount = indexCount > 0 ? indexCount : mesh->indexCount;
    cmdList.Get()->DrawIndexedInstanced(actualIndexCount, instanceCount, startIndex, startVertex, 0);
}

void OpaqueRenderer::EndFrameGBuffer() {}

// ========================================================================
// G-buffer 着色器加载（PS_GBuffer, 复用同一 VS）
// ========================================================================
void OpaqueRenderer::LoadGBufferShader() {
    UINT compileFlags = D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION | D3DCOMPILE_ENABLE_UNBOUNDED_DESCRIPTOR_TABLES;

    Microsoft::WRL::ComPtr<ID3DBlob> errors = nullptr;
    HRESULT hr;

    hr = D3DCompileFromFile(L"Shaders/color.hlsl", nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE, "PS_GBuffer", "ps_5_1",
                            compileFlags, 0, &m_psGBufferBlob, &errors);
    if (FAILED(hr)) {
        if (errors) {
            const char *errMsg = reinterpret_cast<const char *>(errors->GetBufferPointer());
            OutputDebugStringA("=== PS_GBuffer COMPILATION ERROR ===\n");
            OutputDebugStringA(errMsg);
            OutputDebugStringA("=====================================\n");
        }
        throw std::runtime_error("OpaqueRenderer: Failed to compile PS_GBuffer");
    }

    OutputDebugStringW(L"[INFO] OpaqueRenderer: PS_GBuffer compiled successfully\n");
}

// ========================================================================
// G-buffer 根签名（简化版）
//   slot 0: b1  cbPass          (CBV)
//   slot 1: t0,space1            StructuredBuffer<MaterialData> (SRV 描述符表)
//   slot 2: t0                   纹理 SRV (描述符表)
//   slot 3: t12,space1           StructuredBuffer<InstanceData> (SRV)
// ========================================================================
void OpaqueRenderer::CreateGBufferRootSignature() {
    auto device = m_context->GetDevice();

    CD3DX12_ROOT_PARAMETER params[4];

    CD3DX12_DESCRIPTOR_RANGE matRange;
    matRange.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 1);
    CD3DX12_DESCRIPTOR_RANGE texRange;
    texRange.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, UINT_MAX, 0, 2);

    params[0].InitAsConstantBufferView(1, 0, D3D12_SHADER_VISIBILITY_ALL);
    params[1].InitAsDescriptorTable(1, &matRange, D3D12_SHADER_VISIBILITY_PIXEL);
    params[2].InitAsDescriptorTable(1, &texRange, D3D12_SHADER_VISIBILITY_PIXEL);
    params[3].InitAsShaderResourceView(12, 1, D3D12_SHADER_VISIBILITY_ALL);

    // 静态采样器 (s0~s5, 对齐 Common_PBR.hlsl)
    CD3DX12_STATIC_SAMPLER_DESC staticSamplers[6];
    staticSamplers[0].Init(0, D3D12_FILTER_MIN_MAG_MIP_POINT, D3D12_TEXTURE_ADDRESS_MODE_WRAP,
                           D3D12_TEXTURE_ADDRESS_MODE_WRAP, D3D12_TEXTURE_ADDRESS_MODE_WRAP);
    staticSamplers[1].Init(1, D3D12_FILTER_MIN_MAG_MIP_POINT, D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
                           D3D12_TEXTURE_ADDRESS_MODE_CLAMP, D3D12_TEXTURE_ADDRESS_MODE_CLAMP);
    staticSamplers[2].Init(2, D3D12_FILTER_MIN_MAG_MIP_LINEAR, D3D12_TEXTURE_ADDRESS_MODE_WRAP,
                           D3D12_TEXTURE_ADDRESS_MODE_WRAP, D3D12_TEXTURE_ADDRESS_MODE_WRAP);
    staticSamplers[3].Init(3, D3D12_FILTER_MIN_MAG_MIP_LINEAR, D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
                           D3D12_TEXTURE_ADDRESS_MODE_CLAMP, D3D12_TEXTURE_ADDRESS_MODE_CLAMP);
    staticSamplers[4].Init(4, D3D12_FILTER_ANISOTROPIC, D3D12_TEXTURE_ADDRESS_MODE_WRAP,
                           D3D12_TEXTURE_ADDRESS_MODE_WRAP, D3D12_TEXTURE_ADDRESS_MODE_WRAP, 0.0f, 8);
    staticSamplers[5].Init(5, D3D12_FILTER_ANISOTROPIC, D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
                           D3D12_TEXTURE_ADDRESS_MODE_CLAMP, D3D12_TEXTURE_ADDRESS_MODE_CLAMP, 0.0f, 8);

    CD3DX12_ROOT_SIGNATURE_DESC rootSigDesc(4, params, 6, staticSamplers,
                                            D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT);

    Microsoft::WRL::ComPtr<ID3DBlob> serialized, error;
    HRESULT hr = D3D12SerializeRootSignature(&rootSigDesc, D3D_ROOT_SIGNATURE_VERSION_1, &serialized, &error);
    if (error) {
        OutputDebugStringA(reinterpret_cast<const char *>(error->GetBufferPointer()));
    }
    ThrowIfFailed(hr);
    ThrowIfFailed(device->CreateRootSignature(0, serialized->GetBufferPointer(), serialized->GetBufferSize(),
                                              IID_PPV_ARGS(&m_gbufferRootSignature)));
}

// ========================================================================
// G-buffer PSO（4 个 MRT + 深度）
// ========================================================================
void OpaqueRenderer::CreateGBufferPSO() {
    auto device = m_context->GetDevice();

    D3D12_INPUT_ELEMENT_DESC inputLayout[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"TANGENT", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 24, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 36, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0}};

    D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc = {};
    psoDesc.InputLayout = {inputLayout, _countof(inputLayout)};
    psoDesc.pRootSignature = m_gbufferRootSignature.Get();
    psoDesc.VS = {reinterpret_cast<BYTE *>(m_vsBlob->GetBufferPointer()), m_vsBlob->GetBufferSize()};
    psoDesc.PS = {reinterpret_cast<BYTE *>(m_psGBufferBlob->GetBufferPointer()), m_psGBufferBlob->GetBufferSize()};

    psoDesc.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
    psoDesc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
    psoDesc.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
    psoDesc.SampleMask = UINT_MAX;
    psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;

    // 4 个 MRT + 深度
    psoDesc.NumRenderTargets = 4;
    psoDesc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;      // Albedo
    psoDesc.RTVFormats[1] = DXGI_FORMAT_R16G16B16A16_FLOAT;  // Normal
    psoDesc.RTVFormats[2] = DXGI_FORMAT_R8G8B8A8_UNORM;      // Material
    psoDesc.RTVFormats[3] = DXGI_FORMAT_R16G16B16A16_FLOAT;  // WorldPos
    psoDesc.DSVFormat = DXGI_FORMAT_D32_FLOAT;
    psoDesc.SampleDesc.Count = 1;

    ThrowIfFailed(device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&m_gbufferPSO)));
    OutputDebugStringW(L"[INFO] OpaqueRenderer: G-buffer PSO created\n");
}

} // namespace DX12Engine::Renderer
