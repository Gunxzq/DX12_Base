// WaterRenderer.cpp
#include "WaterRenderer.h"
#include "Common/d3dUtil.h"
#include "Renderer/RHI/D3D12DeviceContext.h"
#include "Resource/Geometry/TriangleMesh.h"
#include "Resource/GpuResourceManager.h"
#include "Resource/Manager/GeometryResourceManager.h"
#include <d3dcompiler.h>

using namespace DX12Engine::Renderer;
using namespace DX12Engine::Resource;

namespace DX12Engine::Renderer {

// ========================================================================
// 生命周期管理
// ========================================================================

void WaterRenderer::SetDeviceContext(D3D12DeviceContext *context) { m_context = context; }

void WaterRenderer::Initialize() {
    if (!m_context) {
        throw std::runtime_error("WaterRenderer: Device context not set before Initialize");
    }

    LoadShaders();

    if (!m_vsBlob || !m_psBlob) {
        OutputDebugStringW(L"[ERROR] WaterRenderer: Failed to load shaders!\n");
        throw std::runtime_error("WaterRenderer: Failed to load shaders");
    }

    CreateRootSignature();
    CreatePSO();

    OutputDebugStringW(L"[INFO] WaterRenderer initialized successfully\n");
}

void WaterRenderer::OnResize(uint32_t width, uint32_t height) {
    // 水渲染器不需要特殊的 resize 逻辑
}

void WaterRenderer::Update(float deltaTime) {
    // 水动画可以在着色器中完成，不需要每帧更新 CPU 数据
}

void WaterRenderer::EndFrame() {
    // 无状态需要重置
}

// ========================================================================
// 渲染辅助接口实现
// ========================================================================

void WaterRenderer::BeginFrame(CommandList &cmdList, D3D12_GPU_VIRTUAL_ADDRESS passConstantsAddress,
                               D3D12_GPU_VIRTUAL_ADDRESS lightCBAddress, D3D12_GPU_DESCRIPTOR_HANDLE materialBufferSRV,
                               D3D12_GPU_VIRTUAL_ADDRESS waterCBAddress) {
    if (!m_pso || !m_rootSignature)
        return;

    cmdList.Get()->SetPipelineState(m_pso.Get());
    cmdList.Get()->SetGraphicsRootSignature(m_rootSignature.Get());
    cmdList.Get()->SetGraphicsRootConstantBufferView(1, passConstantsAddress);
    cmdList.Get()->SetGraphicsRootConstantBufferView(2, lightCBAddress);
    cmdList.Get()->SetGraphicsRootConstantBufferView(3, waterCBAddress);

    // 绑定材质数组 SRV (slot 3)
    if (materialBufferSRV.ptr != 0) {
        cmdList.Get()->SetGraphicsRootDescriptorTable(4, materialBufferSRV);
    }
}

void WaterRenderer::DrawWater(CommandList &cmdList, Resource::GeometryHandle geometryHandle,
                              const DirectX::XMMATRIX &worldMatrix, D3D12_GPU_VIRTUAL_ADDRESS objectCBAddress,
                              D3D12_GPU_DESCRIPTOR_HANDLE textureSRV, D3D12_GPU_DESCRIPTOR_HANDLE envMapSRV) {
    if (!m_geometryManager) {
        OutputDebugStringW(L"[ERROR] WaterRenderer::DrawWater - GeometryResourceManager not set!\n");
        return;
    }

    const TriangleMesh *mesh = m_geometryManager->GetGeometry<TriangleMesh>(geometryHandle);
    if (!mesh || !mesh->isGpuReady) {
        return;
    }

    auto &gpuMgr = GpuResourceManager::GetInstance();
    ID3D12Resource *vbResource = gpuMgr.GetResource(mesh->vertexBufferHandle);
    ID3D12Resource *ibResource = gpuMgr.GetResource(mesh->indexBufferHandle);

    if (!vbResource || !ibResource) {
        OutputDebugStringW(L"[ERROR] WaterRenderer::DrawWater - Invalid vertex or index buffer!\n");
        return;
    }

    D3D12_VERTEX_BUFFER_VIEW vbView;
    vbView.BufferLocation = vbResource->GetGPUVirtualAddress();
    vbView.StrideInBytes = mesh->vertexStride;
    vbView.SizeInBytes = static_cast<UINT>(mesh->vertexCount * mesh->vertexStride);

    D3D12_INDEX_BUFFER_VIEW ibView;
    ibView.BufferLocation = ibResource->GetGPUVirtualAddress();
    ibView.Format = mesh->indexFormat;
    ibView.SizeInBytes = static_cast<UINT>(mesh->indexCount * (mesh->indexFormat == DXGI_FORMAT_R32_UINT ? 4 : 2));

    cmdList.Get()->IASetVertexBuffers(0, 1, &vbView);
    cmdList.Get()->IASetIndexBuffer(&ibView);
    cmdList.Get()->IASetPrimitiveTopology(mesh->topology);
    cmdList.Get()->SetGraphicsRootConstantBufferView(0, objectCBAddress);

    // 纹理 SRV (slot 5)
    cmdList.Get()->SetGraphicsRootDescriptorTable(5, textureSRV);

    // 环境贴图 SRV (slot 6, t10)
    if (envMapSRV.ptr != 0) {
        cmdList.Get()->SetGraphicsRootDescriptorTable(6, envMapSRV);
    }

    cmdList.Get()->DrawIndexedInstanced(mesh->indexCount, 1, 0, 0, 0);
}

// ========================================================================
// 内部初始化
// ========================================================================

void WaterRenderer::LoadShaders() {
    UINT compileFlags = D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
    compileFlags |= D3DCOMPILE_ENABLE_UNBOUNDED_DESCRIPTOR_TABLES;

    Microsoft::WRL::ComPtr<ID3DBlob> errors = nullptr;
    HRESULT hr;

    hr = D3DCompileFromFile(L"Shaders/water.hlsl", nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE, "VS", "vs_5_1",
                            compileFlags, 0, &m_vsBlob, &errors);

    if (FAILED(hr)) {
        if (errors) {
            const char *errMsg = reinterpret_cast<const char *>(errors->GetBufferPointer());
            OutputDebugStringA("=== WATER VS COMPILATION ERROR ===\n");
            OutputDebugStringA(errMsg);
            OutputDebugStringA("==================================\n");
        }
        throw std::runtime_error("WaterRenderer: Failed to compile Vertex Shader");
    }

    errors = nullptr;
    hr = D3DCompileFromFile(L"Shaders/water.hlsl", nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE, "PS", "ps_5_1",
                            compileFlags, 0, &m_psBlob, &errors);

    if (FAILED(hr)) {
        if (errors) {
            const char *errMsg = reinterpret_cast<const char *>(errors->GetBufferPointer());
            OutputDebugStringA("=== WATER PS COMPILATION ERROR ===\n");
            OutputDebugStringA(errMsg);
            OutputDebugStringA("==================================\n");
        }
        throw std::runtime_error("WaterRenderer: Failed to compile Pixel Shader");
    }

    OutputDebugStringW(L"[INFO] Water shaders compiled successfully\n");
}

void WaterRenderer::CreateRootSignature() {
    auto device = m_context->GetDevice();

    // ========================================================================
    // 根参数布局 (与 OpaqueRenderer 一致):
    //   slot 0: b0 cbPerObject      (CBV)
    //   slot 1: b1 cbPass           (CBV)
    //   slot 2: b2 cbLights         (CBV)
    //   slot 3: b3 cbWater          (CBV)
    //   slot 4: t0,space1           StructuredBuffer<MaterialData> (SRV 描述符表)
    //   slot 5: t0                  纹理 SRV (描述符表)
    //   slot 6: t10                 环境贴图 SRV (描述符表)
    // ========================================================================
    CD3DX12_ROOT_PARAMETER slotRootParameter[7];

    CD3DX12_DESCRIPTOR_RANGE materialBufferRange;
    materialBufferRange.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 1, D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND);

    CD3DX12_DESCRIPTOR_RANGE texTable;
    texTable.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND);

    CD3DX12_DESCRIPTOR_RANGE envMapTable;
    envMapTable.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 10, 0, D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND);

    slotRootParameter[0].InitAsConstantBufferView(0, 0, D3D12_SHADER_VISIBILITY_ALL);                   // b0
    slotRootParameter[1].InitAsConstantBufferView(1, 0, D3D12_SHADER_VISIBILITY_ALL);                   // b1
    slotRootParameter[2].InitAsConstantBufferView(2, 0, D3D12_SHADER_VISIBILITY_ALL);                   // b2
    slotRootParameter[3].InitAsConstantBufferView(3, 0, D3D12_SHADER_VISIBILITY_ALL);                   // b3
    slotRootParameter[4].InitAsDescriptorTable(1, &materialBufferRange, D3D12_SHADER_VISIBILITY_PIXEL); // t0,space1
    slotRootParameter[5].InitAsDescriptorTable(1, &texTable, D3D12_SHADER_VISIBILITY_PIXEL);            // t0
    slotRootParameter[6].InitAsDescriptorTable(1, &envMapTable, D3D12_SHADER_VISIBILITY_PIXEL);         // t10

    // ========================================================================
    // 静态采样器 (与 OpaqueRenderer 一致)
    // ========================================================================
    CD3DX12_STATIC_SAMPLER_DESC staticSamplers[7];

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
    staticSamplers[6].Init(10, D3D12_FILTER_MIN_MAG_MIP_LINEAR, D3D12_TEXTURE_ADDRESS_MODE_WRAP,
                           D3D12_TEXTURE_ADDRESS_MODE_WRAP, D3D12_TEXTURE_ADDRESS_MODE_WRAP);

    CD3DX12_ROOT_SIGNATURE_DESC rootSigDesc(7, slotRootParameter, 7, staticSamplers,
                                            D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT);

    Microsoft::WRL::ComPtr<ID3DBlob> serializedRootSig = nullptr;
    Microsoft::WRL::ComPtr<ID3DBlob> errorBlob = nullptr;
    HRESULT hr =
        D3D12SerializeRootSignature(&rootSigDesc, D3D_ROOT_SIGNATURE_VERSION_1, &serializedRootSig, &errorBlob);

    if (errorBlob != nullptr) {
        OutputDebugStringA(reinterpret_cast<const char *>(errorBlob->GetBufferPointer()));
    }
    ThrowIfFailed(hr);

    ThrowIfFailed(device->CreateRootSignature(0, serializedRootSig->GetBufferPointer(),
                                              serializedRootSig->GetBufferSize(), IID_PPV_ARGS(&m_rootSignature)));
}

void WaterRenderer::CreatePSO() {
    auto device = m_context->GetDevice();

    // 输入布局与 OpaqueRenderer 一致
    D3D12_INPUT_ELEMENT_DESC inputLayout[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"TANGENT", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 24, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 36, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0}};

    D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc = {};
    psoDesc.InputLayout = {inputLayout, _countof(inputLayout)};
    psoDesc.pRootSignature = m_rootSignature.Get();
    psoDesc.VS = {reinterpret_cast<BYTE *>(m_vsBlob->GetBufferPointer()), m_vsBlob->GetBufferSize()};
    psoDesc.PS = {reinterpret_cast<BYTE *>(m_psBlob->GetBufferPointer()), m_psBlob->GetBufferSize()};

    // 光栅化状态
    psoDesc.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);

    // 混合状态 - 透明度混合
    D3D12_BLEND_DESC blendDesc = {};
    blendDesc.RenderTarget[0].BlendEnable = TRUE;
    blendDesc.RenderTarget[0].SrcBlend = D3D12_BLEND_SRC_ALPHA;
    blendDesc.RenderTarget[0].DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
    blendDesc.RenderTarget[0].BlendOp = D3D12_BLEND_OP_ADD;
    blendDesc.RenderTarget[0].SrcBlendAlpha = D3D12_BLEND_ONE;
    blendDesc.RenderTarget[0].DestBlendAlpha = D3D12_BLEND_ZERO;
    blendDesc.RenderTarget[0].BlendOpAlpha = D3D12_BLEND_OP_ADD;
    blendDesc.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    psoDesc.BlendState = blendDesc;

    // 深度状态 - 关闭深度写入
    D3D12_DEPTH_STENCIL_DESC depthStencilDesc = {};
    depthStencilDesc.DepthEnable = TRUE;
    depthStencilDesc.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO; // 关键：不写入深度
    depthStencilDesc.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
    depthStencilDesc.StencilEnable = FALSE;
    psoDesc.DepthStencilState = depthStencilDesc;

    // D3D12_DEPTH_STENCIL_DESC depthStencilDesc = {};
    // depthStencilDesc.DepthEnable = FALSE; // 关闭深度测试
    // depthStencilDesc.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
    // depthStencilDesc.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    // depthStencilDesc.StencilEnable = FALSE;
    // psoDesc.DepthStencilState = depthStencilDesc;

    psoDesc.SampleMask = UINT_MAX;
    psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    psoDesc.NumRenderTargets = 1;
    psoDesc.RTVFormats[0] = m_context->GetBackBufferFormat();
    psoDesc.DSVFormat = m_context->GetDepthStencilFormat();
    psoDesc.SampleDesc.Count = m_context->Is4xMsaaEnabled() ? 4 : 1;
    psoDesc.SampleDesc.Quality = m_context->Is4xMsaaEnabled() ? (m_context->Get4xMsaaQuality() - 1) : 0;

    ThrowIfFailed(device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&m_pso)));

    OutputDebugStringW(L"[INFO] Water PSO created successfully\n");
}

} // namespace DX12Engine::Renderer