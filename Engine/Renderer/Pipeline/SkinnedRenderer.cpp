#include "SkinnedRenderer.h"
#include "Common/d3dUtil.h"
#include "Renderer/RHI/D3D12DeviceContext.h"
#include "Resource/Geometry/TriangleMesh.h"
#include "Resource/GpuResourceManager.h"
#include "Resource/Manager/GeometryResourceManager.h"
#include <DirectXMath.h>
#include <d3dcompiler.h>

using namespace DX12Engine::Renderer;
using namespace DX12Engine::Resource;

namespace DX12Engine::Renderer {

// ========================================================================
// IRenderer 接口实现
// ========================================================================

void SkinnedRenderer::SetDeviceContext(D3D12DeviceContext *context) { m_context = context; }

void SkinnedRenderer::Initialize() {
    if (!m_context) {
        OutputDebugStringW(L"[ERROR] SkinnedRenderer::Initialize - DeviceContext not set!\n");
        return;
    }

    // 编译蒙皮顶点着色器（G-buffer PSO 使用）
    {
        UINT compileFlags = D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION | D3DCOMPILE_ENABLE_UNBOUNDED_DESCRIPTOR_TABLES;
        Microsoft::WRL::ComPtr<ID3DBlob> errors;
        HRESULT hr = D3DCompileFromFile(L"Shaders/skinned.hlsl", nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE,
                                        "VS", "vs_5_1", compileFlags, 0, &m_vsBlob, &errors);
        if (FAILED(hr)) {
            if (errors) OutputDebugStringA(reinterpret_cast<const char *>(errors->GetBufferPointer()));
            ThrowIfFailed(hr);
        }
    }

    LoadGBufferShader();
    CreateGBufferRootSignature();
    CreateGBufferPSO();
    OutputDebugStringW(L"[INFO] SkinnedRenderer initialized (GBuffer only)\n");
}

void SkinnedRenderer::OnResize(uint32_t width, uint32_t height) {
    (void)width;
    (void)height;
}

void SkinnedRenderer::Update(float deltaTime) { (void)deltaTime; }

// ========================================================================
// G-buffer 支持
// ========================================================================

void SkinnedRenderer::LoadGBufferShader() {
    UINT compileFlags = D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION | D3DCOMPILE_ENABLE_UNBOUNDED_DESCRIPTOR_TABLES;
    Microsoft::WRL::ComPtr<ID3DBlob> errors;
    HRESULT hr = D3DCompileFromFile(L"Shaders/skinned.hlsl", nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE,
                                    "PS_GBuffer", "ps_5_1", compileFlags, 0, &m_psGBufferBlob, &errors);
    if (FAILED(hr)) {
        if (errors) OutputDebugStringA(reinterpret_cast<const char *>(errors->GetBufferPointer()));
        ThrowIfFailed(hr);
    }
}

void SkinnedRenderer::CreateGBufferRootSignature() {
    auto device = m_context->GetDevice();
    // 保持与前向根签名相同的槽位索引，便于 DrawItems 复用
    //   slot 0: b1 cbPass
    //   slot 1: (未使用, 占位保持索引对齐)
    //   slot 2: t0,space1 MaterialData SRV
    //   slot 3: t0,space2 Texture heap
    //   slot 4: t12,space1 InstanceData SRV
    //   slot 5: t13,space1 BoneTransforms SRV
    CD3DX12_ROOT_PARAMETER params[6];
    params[0].InitAsConstantBufferView(1, 0, D3D12_SHADER_VISIBILITY_ALL);
    CD3DX12_DESCRIPTOR_RANGE dummyRange;
    dummyRange.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0);
    params[1].InitAsDescriptorTable(1, &dummyRange, D3D12_SHADER_VISIBILITY_PIXEL);
    CD3DX12_DESCRIPTOR_RANGE matRange;
    matRange.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 1);
    params[2].InitAsDescriptorTable(1, &matRange, D3D12_SHADER_VISIBILITY_PIXEL);
    CD3DX12_DESCRIPTOR_RANGE texRange;
    texRange.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, UINT_MAX, 0, 2);
    params[3].InitAsDescriptorTable(1, &texRange, D3D12_SHADER_VISIBILITY_PIXEL);
    params[4].InitAsShaderResourceView(12, 1, D3D12_SHADER_VISIBILITY_VERTEX);
    params[5].InitAsShaderResourceView(13, 1, D3D12_SHADER_VISIBILITY_VERTEX);

    CD3DX12_STATIC_SAMPLER_DESC samplers[3];
    samplers[0].Init(0, D3D12_FILTER_MIN_MAG_MIP_POINT, D3D12_TEXTURE_ADDRESS_MODE_WRAP, D3D12_TEXTURE_ADDRESS_MODE_WRAP, D3D12_TEXTURE_ADDRESS_MODE_WRAP);
    samplers[1].Init(1, D3D12_FILTER_MIN_MAG_MIP_LINEAR, D3D12_TEXTURE_ADDRESS_MODE_WRAP, D3D12_TEXTURE_ADDRESS_MODE_WRAP, D3D12_TEXTURE_ADDRESS_MODE_WRAP);
    samplers[2].Init(2, D3D12_FILTER_ANISOTROPIC, D3D12_TEXTURE_ADDRESS_MODE_WRAP, D3D12_TEXTURE_ADDRESS_MODE_WRAP, D3D12_TEXTURE_ADDRESS_MODE_WRAP, 0.0f, 8);

    CD3DX12_ROOT_SIGNATURE_DESC rsDesc(6, params, 3, samplers, D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT);
    Microsoft::WRL::ComPtr<ID3DBlob> sig, err;
    D3D12SerializeRootSignature(&rsDesc, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &err);
    if (err) OutputDebugStringA(reinterpret_cast<const char *>(err->GetBufferPointer()));
    device->CreateRootSignature(0, sig->GetBufferPointer(), sig->GetBufferSize(), IID_PPV_ARGS(&m_gbufferRootSignature));
}

void SkinnedRenderer::CreateGBufferPSO() {
    auto device = m_context->GetDevice();
    D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc = {};
    psoDesc.pRootSignature = m_gbufferRootSignature.Get();
    psoDesc.VS = {m_vsBlob->GetBufferPointer(), m_vsBlob->GetBufferSize()};
    psoDesc.PS = {m_psGBufferBlob->GetBufferPointer(), m_psGBufferBlob->GetBufferSize()};
    psoDesc.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
    psoDesc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
    psoDesc.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
    psoDesc.SampleMask = UINT_MAX;
    psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    psoDesc.NumRenderTargets = 4;
    psoDesc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    psoDesc.RTVFormats[1] = DXGI_FORMAT_R16G16B16A16_FLOAT;
    psoDesc.RTVFormats[2] = DXGI_FORMAT_R8G8B8A8_UNORM;
    psoDesc.RTVFormats[3] = DXGI_FORMAT_R16G16B16A16_FLOAT;
    psoDesc.DSVFormat = DXGI_FORMAT_D32_FLOAT;
    psoDesc.SampleDesc.Count = 1;
    // Input layout: PosL(12) + TangentU(12) + NormalL(12) + TexC(8) + BoneWeights(16) + BoneIndices(16)
    D3D12_INPUT_ELEMENT_DESC inputLayout[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"TANGENT", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 24, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 36, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"BLENDWEIGHTS", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 44, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"BLENDINDICES", 0, DXGI_FORMAT_R8G8B8A8_UINT, 0, 60, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
    };
    psoDesc.InputLayout = {inputLayout, 6};
    device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&m_gbufferPSO));
    OutputDebugStringW(L"[INFO] SkinnedRenderer: GBuffer PSO created\n");
}

// ========================================================================
// G-buffer 渲染
// ========================================================================

void SkinnedRenderer::BeginFrameGBuffer(CommandList &cmdList, D3D12_GPU_VIRTUAL_ADDRESS passConstantsAddress,
                                        D3D12_GPU_DESCRIPTOR_HANDLE materialBufferSRV,
                                        D3D12_GPU_DESCRIPTOR_HANDLE textureHeapStart) {
    if (!m_gbufferRootSignature) return;
    cmdList.Get()->SetGraphicsRootSignature(m_gbufferRootSignature.Get());
    cmdList.Get()->SetGraphicsRootConstantBufferView(0, passConstantsAddress);
    // slot 1: dummy（占位）
    if (materialBufferSRV.ptr != 0)
        cmdList.Get()->SetGraphicsRootDescriptorTable(2, materialBufferSRV);
    if (textureHeapStart.ptr != 0)
        cmdList.Get()->SetGraphicsRootDescriptorTable(3, textureHeapStart);
}

void SkinnedRenderer::DrawGBuffer(CommandList &cmdList, const TRenderQueue<SkinnedRenderItem> &queue) {
    DrawItems(cmdList, queue, m_gbufferPSO.Get());
}

void SkinnedRenderer::EndFrameGBuffer() {}

// ========================================================================
// DrawItems — 内部：遍历队列，设置 PSO + 逐实例数据 + 绘制
// ========================================================================

void SkinnedRenderer::DrawItems(CommandList &cmdList, const TRenderQueue<SkinnedRenderItem> &queue,
                                ID3D12PipelineState *pso) {
    if (!m_geometryManager || !pso)
        return;

    auto &gpuMgr = GpuResourceManager::GetInstance();

    cmdList.Get()->SetPipelineState(pso);

    for (const auto &item : queue) {
        if (!item.IsValid())
            continue;

        const TriangleMesh *mesh = m_geometryManager->GetGeometry<TriangleMesh>(item.geometryHandle);
        if (!mesh || !mesh->isGpuReady)
            continue;

        ID3D12Resource *vbResource = gpuMgr.GetResource(mesh->vertexBufferHandle);
        ID3D12Resource *ibResource = gpuMgr.GetResource(mesh->indexBufferHandle);
        if (!vbResource || !ibResource)
            continue;

        // slot 4: InstanceData SRV
        cmdList.Get()->SetGraphicsRootShaderResourceView(4, item.instanceBuffer);
        // slot 5: BoneTransforms SRV
        cmdList.Get()->SetGraphicsRootShaderResourceView(5, item.boneBufferAddress);

        // 顶点/索引缓冲区
        D3D12_VERTEX_BUFFER_VIEW vbView;
        vbView.BufferLocation = vbResource->GetGPUVirtualAddress();
        vbView.StrideInBytes = mesh->vertexStride;
        vbView.SizeInBytes = static_cast<UINT>(mesh->vertexCount * mesh->vertexStride);
        cmdList.Get()->IASetVertexBuffers(0, 1, &vbView);

        D3D12_INDEX_BUFFER_VIEW ibView;
        ibView.BufferLocation = ibResource->GetGPUVirtualAddress();
        ibView.Format = mesh->indexFormat;
        ibView.SizeInBytes = static_cast<UINT>(mesh->indexCount * (mesh->indexFormat == DXGI_FORMAT_R32_UINT ? 4 : 2));
        cmdList.Get()->IASetIndexBuffer(&ibView);
        cmdList.Get()->IASetPrimitiveTopology(mesh->topology);

        cmdList.Get()->DrawIndexedInstanced(item.indexCount > 0 ? item.indexCount : mesh->indexCount,
                                            item.instanceCount, item.startIndex, item.startVertex, 0);
    }
}

} // namespace DX12Engine::Renderer
