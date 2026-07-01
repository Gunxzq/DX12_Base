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
    LoadShaders();
    CreateRootSignature();
    CreatePSOs();
    OutputDebugStringW(L"[INFO] SkinnedRenderer initialized\n");
}

void SkinnedRenderer::OnResize(uint32_t width, uint32_t height) {
    (void)width;
    (void)height;
}

void SkinnedRenderer::Update(float deltaTime) { (void)deltaTime; }

void SkinnedRenderer::EndFrame() {}

// ========================================================================
// 着色器加载
// ========================================================================

void SkinnedRenderer::LoadShaders() {
    UINT compileFlags = D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION | D3DCOMPILE_ENABLE_UNBOUNDED_DESCRIPTOR_TABLES;

    Microsoft::WRL::ComPtr<ID3DBlob> errors;

    HRESULT hr = D3DCompileFromFile(L"Shaders/skinned.hlsl", nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE, "VS", "vs_5_1",
                                    compileFlags, 0, &m_vsBlob, &errors);
    if (FAILED(hr)) {
        if (errors) {
            OutputDebugStringA(reinterpret_cast<const char *>(errors->GetBufferPointer()));
        }
        ThrowIfFailed(hr);
    }

    errors = nullptr;
    hr = D3DCompileFromFile(L"Shaders/skinned.hlsl", nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE, "PS", "ps_5_1",
                            compileFlags, 0, &m_psBlob, &errors);
    if (FAILED(hr)) {
        if (errors) {
            OutputDebugStringA(reinterpret_cast<const char *>(errors->GetBufferPointer()));
        }
        ThrowIfFailed(hr);
    }
}

// ========================================================================
// 根签名
//   slot 0: b1  cbPass             (CBV, ALL)
//   slot 1: b2  cbLights           (CBV, PS)
//   slot 2: t0,space1  MaterialData StructuredBuffer (SRV, PS)
//   slot 3: t0           Texture2D[] 纹理数组 (SRV, PS)
//   slot 4: t12,space1  InstanceData StructuredBuffer (SRV, VS)
//   slot 5: t13,space1  BoneTransforms StructuredBuffer (SRV, VS)
//   slot 6: t10         环境贴图 TextureCube (SRV, PS)
// ========================================================================

void SkinnedRenderer::CreateRootSignature() {
    auto device = m_context->GetDevice();

    CD3DX12_ROOT_PARAMETER slotRootParameter[7];

    // slot 0: b1 cbPass
    slotRootParameter[0].InitAsConstantBufferView(1, 0, D3D12_SHADER_VISIBILITY_ALL);
    // slot 1: b2 cbLights
    slotRootParameter[1].InitAsConstantBufferView(2, 0, D3D12_SHADER_VISIBILITY_PIXEL);

    // slot 2: t0,space1 MaterialData StructuredBuffer (描述符表)
    CD3DX12_DESCRIPTOR_RANGE materialRange;
    materialRange.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 1);
    slotRootParameter[2].InitAsDescriptorTable(1, &materialRange, D3D12_SHADER_VISIBILITY_PIXEL);

    // slot 3: t0 纹理数组 (描述符表)
    CD3DX12_DESCRIPTOR_RANGE texRange;
    texRange.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, UINT_MAX, 0, 2);
    slotRootParameter[3].InitAsDescriptorTable(1, &texRange, D3D12_SHADER_VISIBILITY_PIXEL);

    // slot 4: t12,space1 InstanceData SRV
    slotRootParameter[4].InitAsShaderResourceView(12, 1, D3D12_SHADER_VISIBILITY_VERTEX);
    // slot 5: t13,space1 BoneTransforms SRV
    slotRootParameter[5].InitAsShaderResourceView(13, 1, D3D12_SHADER_VISIBILITY_VERTEX);

    // slot 6: t10 环境贴图 (描述符表)
    CD3DX12_DESCRIPTOR_RANGE envRange;
    envRange.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 10, 0);
    slotRootParameter[6].InitAsDescriptorTable(1, &envRange, D3D12_SHADER_VISIBILITY_PIXEL);

    // 静态采样器
    CD3DX12_STATIC_SAMPLER_DESC staticSamplers[4];
    staticSamplers[0].Init(0, D3D12_FILTER_MIN_MAG_MIP_POINT, D3D12_TEXTURE_ADDRESS_MODE_WRAP,
                           D3D12_TEXTURE_ADDRESS_MODE_WRAP, D3D12_TEXTURE_ADDRESS_MODE_WRAP);
    staticSamplers[1].Init(1, D3D12_FILTER_MIN_MAG_MIP_LINEAR, D3D12_TEXTURE_ADDRESS_MODE_WRAP,
                           D3D12_TEXTURE_ADDRESS_MODE_WRAP, D3D12_TEXTURE_ADDRESS_MODE_WRAP);
    staticSamplers[2].Init(2, D3D12_FILTER_ANISOTROPIC, D3D12_TEXTURE_ADDRESS_MODE_WRAP,
                           D3D12_TEXTURE_ADDRESS_MODE_WRAP, D3D12_TEXTURE_ADDRESS_MODE_WRAP, 0.0f, 8);
    staticSamplers[3].Init(10, D3D12_FILTER_MIN_MAG_MIP_LINEAR, D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
                           D3D12_TEXTURE_ADDRESS_MODE_CLAMP, D3D12_TEXTURE_ADDRESS_MODE_CLAMP);

    CD3DX12_ROOT_SIGNATURE_DESC rootSigDesc(7, slotRootParameter, 4, staticSamplers,
                                            D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT);

    Microsoft::WRL::ComPtr<ID3DBlob> serializedRootSig = nullptr;
    Microsoft::WRL::ComPtr<ID3DBlob> errorBlob = nullptr;
    HRESULT hr =
        D3D12SerializeRootSignature(&rootSigDesc, D3D_ROOT_SIGNATURE_VERSION_1, &serializedRootSig, &errorBlob);
    if (errorBlob) {
        OutputDebugStringA(reinterpret_cast<const char *>(errorBlob->GetBufferPointer()));
    }
    ThrowIfFailed(hr);
    ThrowIfFailed(device->CreateRootSignature(0, serializedRootSig->GetBufferPointer(),
                                              serializedRootSig->GetBufferSize(), IID_PPV_ARGS(&m_rootSignature)));
}

// ========================================================================
// PSO 创建 — 两个变体：不透明 / 透明
// ========================================================================

void SkinnedRenderer::CreatePSOs() {
    if (!m_rootSignature || !m_vsBlob || !m_psBlob)
        return;

    auto device = m_context->GetDevice();

    // 输入布局：M3dVertex (64 bytes, 含骨骼权重/索引)
    // .m3d 存储顺序: Pos(0) | TangentU(12) | Normal(24) | TexC(36) | BoneWeights(44) | BoneIndices(60)
    D3D12_INPUT_ELEMENT_DESC inputLayout[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"TANGENT", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 24, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 36, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"BLENDWEIGHTS", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 44, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"BLENDINDICES", 0, DXGI_FORMAT_R8G8B8A8_UINT, 0, 60, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
    };

    // --- PSO 公共部分 ---
    D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc = {};
    psoDesc.InputLayout = {inputLayout, _countof(inputLayout)};
    psoDesc.pRootSignature = m_rootSignature.Get();
    psoDesc.VS = {reinterpret_cast<BYTE *>(m_vsBlob->GetBufferPointer()), m_vsBlob->GetBufferSize()};
    psoDesc.PS = {reinterpret_cast<BYTE *>(m_psBlob->GetBufferPointer()), m_psBlob->GetBufferSize()};
    psoDesc.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
    psoDesc.RasterizerState.FrontCounterClockwise = TRUE; // .m3d 模型使用逆时针绕序（OpenGL 习惯）
    psoDesc.SampleMask = UINT_MAX;
    psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    psoDesc.NumRenderTargets = 1;
    psoDesc.RTVFormats[0] = m_context->GetBackBufferFormat();
    psoDesc.DSVFormat = m_context->GetDepthStencilFormat();
    psoDesc.SampleDesc.Count = m_context->Is4xMsaaEnabled() ? 4 : 1;
    psoDesc.SampleDesc.Quality = m_context->Is4xMsaaEnabled() ? (m_context->Get4xMsaaQuality() - 1) : 0;

    // --- 变体 1：不透明蒙皮 ---
    {
        D3D12_DEPTH_STENCIL_DESC ds = {};
        ds.DepthEnable = TRUE;
        ds.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
        ds.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
        ds.StencilEnable = FALSE;

        D3D12_BLEND_DESC blend = {};
        blend.RenderTarget[0].BlendEnable = FALSE;
        blend.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;

        psoDesc.DepthStencilState = ds;
        psoDesc.BlendState = blend;

        HRESULT hr = device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&m_psoOpaque));
        if (FAILED(hr)) {
            OutputDebugStringW(L"[ERROR] SkinnedRenderer: Failed to create opaque PSO\n");
        }
    }

    // --- 变体 2：透明蒙皮 ---
    {
        D3D12_DEPTH_STENCIL_DESC ds = {};
        ds.DepthEnable = TRUE;
        ds.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO; // 透明不写深度
        ds.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
        ds.StencilEnable = FALSE;

        D3D12_BLEND_DESC blend = {};
        blend.RenderTarget[0].BlendEnable = TRUE;
        blend.RenderTarget[0].SrcBlend = D3D12_BLEND_SRC_ALPHA;
        blend.RenderTarget[0].DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
        blend.RenderTarget[0].BlendOp = D3D12_BLEND_OP_ADD;
        blend.RenderTarget[0].SrcBlendAlpha = D3D12_BLEND_ONE;
        blend.RenderTarget[0].DestBlendAlpha = D3D12_BLEND_ZERO;
        blend.RenderTarget[0].BlendOpAlpha = D3D12_BLEND_OP_ADD;
        blend.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;

        psoDesc.DepthStencilState = ds;
        psoDesc.BlendState = blend;

        HRESULT hr = device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&m_psoTransparent));
        if (FAILED(hr)) {
            OutputDebugStringW(L"[ERROR] SkinnedRenderer: Failed to create transparent PSO\n");
        }
    }
}

// ========================================================================
// BeginFrame — 绑定根签名 + 全局资源（一次设置，持续到 EndFrame）
// ========================================================================

void SkinnedRenderer::BeginFrame(CommandList &cmdList, D3D12_GPU_VIRTUAL_ADDRESS passConstantsAddress,
                                 D3D12_GPU_VIRTUAL_ADDRESS lightCBAddress,
                                 D3D12_GPU_DESCRIPTOR_HANDLE materialBufferSRV,
                                 D3D12_GPU_DESCRIPTOR_HANDLE textureHeapStart, D3D12_GPU_DESCRIPTOR_HANDLE envMapSRV) {
    if (!m_rootSignature)
        return;

    cmdList.Get()->SetGraphicsRootSignature(m_rootSignature.Get());

    // slot 0: cbPass
    cmdList.Get()->SetGraphicsRootConstantBufferView(0, passConstantsAddress);
    // slot 1: cbLights
    cmdList.Get()->SetGraphicsRootConstantBufferView(1, lightCBAddress);

    // slot 2: MaterialData SRV
    if (materialBufferSRV.ptr != 0) {
        cmdList.Get()->SetGraphicsRootDescriptorTable(2, materialBufferSRV);
    }

    // slot 3: 纹理数组
    if (textureHeapStart.ptr != 0) {
        cmdList.Get()->SetGraphicsRootDescriptorTable(3, textureHeapStart);
    }

    // slot 6: 环境贴图
    if (envMapSRV.ptr != 0) {
        cmdList.Get()->SetGraphicsRootDescriptorTable(6, envMapSRV);
    }
}

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

void SkinnedRenderer::DrawOpaque(CommandList &cmdList, const TRenderQueue<SkinnedRenderItem> &queue) {
    DrawItems(cmdList, queue, m_psoOpaque.Get());
}

void SkinnedRenderer::DrawTransparent(CommandList &cmdList, const TRenderQueue<SkinnedRenderItem> &queue) {
    DrawItems(cmdList, queue, m_psoTransparent.Get());
}

} // namespace DX12Engine::Renderer
