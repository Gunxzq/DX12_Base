// TerrainRenderer.cpp
#include "TerrainRenderer.h"
#include "Common/d3dUtil.h"
#include "Renderer/RHI/D3D12DeviceContext.h"
#include "Resource/Geometry/PatchMesh.h"
#include "Resource/GpuResourceManager.h"
#include "Resource/Manager/GeometryResourceManager.h"
#include "Resource/Manager/MaterialManager.h"
#include <d3dcompiler.h>

using namespace DirectX;
using namespace DX12Engine::Resource;

namespace DX12Engine::Renderer {

// ============================================================================
// 生命周期管理
// ============================================================================

void TerrainRenderer::SetDeviceContext(D3D12DeviceContext *context) { m_context = context; }

void TerrainRenderer::Initialize() {
    if (!m_context) {
        throw std::runtime_error("TerrainRenderer: Device context not set before Initialize");
    }

    LoadShaders();
    CreateRootSignature();
    CreatePSOs();

    OutputDebugStringW(L"[INFO] TerrainRenderer initialized successfully\n");
}

void TerrainRenderer::OnResize(uint32_t width, uint32_t height) {
    (void)width;
    (void)height;
}

void TerrainRenderer::Update(float deltaTime) { (void)deltaTime; }

void TerrainRenderer::EndFrame() {
    // 无每帧清理需求
}

// ============================================================================
// 渲染接口
// ============================================================================

void TerrainRenderer::BeginFrame(CommandList &cmdList, D3D12_GPU_VIRTUAL_ADDRESS passConstantsAddress,
                                 D3D12_GPU_VIRTUAL_ADDRESS lightCBAddress,
                                 D3D12_GPU_DESCRIPTOR_HANDLE materialBufferSRV) {
    if (!m_psoStandard || !m_rootSignature) {
        OutputDebugStringW(L"[ERROR] TerrainRenderer::BeginFrame: PSO or RootSignature not initialized\n");
        return;
    }

    // 设置通用资源（Pass、Lights、材质）
    BindCommonResources(cmdList, passConstantsAddress, lightCBAddress, materialBufferSRV);
}

void TerrainRenderer::DrawTerrain(CommandList &cmdList, const TerrainRenderItem &item) {
    static int drawLogCount = 0;

    if (!m_geometryManager) {
        OutputDebugStringW(L"[ERROR] TerrainRenderer::DrawTerrain - GeometryResourceManager not set!\n");
        return;
    }

    // 获取 PatchMesh（支持曲面细分）
    const PatchMesh *mesh = m_geometryManager->GetGeometry<PatchMesh>(item.geometryHandle);
    if (!mesh) {
        if (drawLogCount++ < 3) {
            OutputDebugStringW(L"[ERROR] TerrainRenderer::DrawTerrain - GetGeometry<PatchMesh> returned nullptr!\n");
        }
        return;
    }
    if (!mesh->isGpuReady) {
        if (drawLogCount++ < 3) {
            OutputDebugStringW(L"[ERROR] TerrainRenderer::DrawTerrain - PatchMesh isGpuReady is false!\n");
        }
        return;
    }

    auto &gpuMgr = GpuResourceManager::GetInstance();
    ID3D12Resource *vbResource = gpuMgr.GetResource(mesh->vertexBufferHandle);
    ID3D12Resource *ibResource = gpuMgr.GetResource(mesh->indexBufferHandle);

    if (!vbResource || !ibResource) {
        OutputDebugStringW(L"[ERROR] TerrainRenderer::DrawTerrain - Invalid vertex or index buffer!\n");
        return;
    }

    // 设置顶点缓冲区
    D3D12_VERTEX_BUFFER_VIEW vbView;
    vbView.BufferLocation = vbResource->GetGPUVirtualAddress();
    vbView.StrideInBytes = mesh->vertexStride;
    vbView.SizeInBytes = static_cast<UINT>(mesh->vertexCount * mesh->vertexStride);

    // 设置索引缓冲区
    D3D12_INDEX_BUFFER_VIEW ibView;
    ibView.BufferLocation = ibResource->GetGPUVirtualAddress();
    ibView.Format = mesh->indexFormat;

    UINT indexCount = mesh->indexCount;
    ibView.SizeInBytes = static_cast<UINT>(mesh->indexCount * (mesh->indexFormat == DXGI_FORMAT_R32_UINT ? 4 : 2));
    cmdList.Get()->IASetPrimitiveTopology(mesh->GetPrimitiveTopology());

    cmdList.Get()->IASetVertexBuffers(0, 1, &vbView);
    cmdList.Get()->IASetIndexBuffer(&ibView);

    // 选择 PSO（根据是否需要实例化）
    if (item.instanceCount > 1) {
        cmdList.Get()->SetPipelineState(m_psoInstanced.Get());
        cmdList.Get()->SetGraphicsRootShaderResourceView(8, item.instanceBufferAddress);
        cmdList.Get()->SetGraphicsRootDescriptorTable(4, item.heightMapSRV);
        cmdList.Get()->DrawIndexedInstanced(indexCount, item.instanceCount, 0, 0, 0);
    } else {
        cmdList.Get()->SetPipelineState(m_psoStandard.Get());
        cmdList.Get()->SetGraphicsRootConstantBufferView(0, item.instanceBufferAddress);
        cmdList.Get()->SetGraphicsRootDescriptorTable(4, item.heightMapSRV);
        cmdList.Get()->DrawIndexedInstanced(indexCount, 1, 0, 0, 0);
    }
}

// ============================================================================
// PSO 切换
// ============================================================================

void TerrainRenderer::SetStandardPSO(CommandList &cmdList) const {
    if (m_psoStandard) {
        cmdList.Get()->SetPipelineState(m_psoStandard.Get());
    }
}

void TerrainRenderer::SetInstancedPSO(CommandList &cmdList) const {
    if (m_psoInstanced) {
        cmdList.Get()->SetPipelineState(m_psoInstanced.Get());
    }
}

// ============================================================================
// 内部初始化
// ============================================================================

void TerrainRenderer::LoadShaders() {
    UINT compileFlags = D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
    compileFlags |= D3DCOMPILE_ENABLE_UNBOUNDED_DESCRIPTOR_TABLES;
    Microsoft::WRL::ComPtr<ID3DBlob> errors = nullptr;
    HRESULT hr;

    // 标准 VS（不使用实例化）
    hr = D3DCompileFromFile(L"Shaders/Terrain.hlsl", nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE, "VS", "vs_5_1",
                            compileFlags, 0, &m_vsStandard, &errors);
    if (FAILED(hr)) {
        if (errors) {
            OutputDebugStringA(reinterpret_cast<const char *>(errors->GetBufferPointer()));
        }
        throw std::runtime_error("TerrainRenderer: Failed to compile Standard VS");
    }

    // 实例化 VS（使用 SV_InstanceID）
    D3D_SHADER_MACRO instancedDefines[] = {{"USE_INSTANCING", "1"}, {nullptr, nullptr}};
    hr = D3DCompileFromFile(L"Shaders/Terrain.hlsl", instancedDefines, D3D_COMPILE_STANDARD_FILE_INCLUDE, "VS_Instanced",
                            "vs_5_1", compileFlags, 0, &m_vsInstanced, &errors);
    if (FAILED(hr)) {
        if (errors) {
            OutputDebugStringA(reinterpret_cast<const char *>(errors->GetBufferPointer()));
        }
        throw std::runtime_error("TerrainRenderer: Failed to compile Instanced VS");
    }

    // Hull Shader（曲面细分必需）
    hr = D3DCompileFromFile(L"Shaders/Terrain.hlsl", nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE, "HS", "hs_5_1",
                            compileFlags, 0, &m_hs, &errors);
    if (FAILED(hr)) {
        if (errors) {
            OutputDebugStringA(reinterpret_cast<const char *>(errors->GetBufferPointer()));
        }
        throw std::runtime_error("TerrainRenderer: Failed to compile HS");
    }

    // Domain Shader（曲面细分必需）
    hr = D3DCompileFromFile(L"Shaders/Terrain.hlsl", nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE, "DS", "ds_5_1",
                            compileFlags, 0, &m_ds, &errors);
    if (FAILED(hr)) {
        if (errors) {
            OutputDebugStringA(reinterpret_cast<const char *>(errors->GetBufferPointer()));
        }
        throw std::runtime_error("TerrainRenderer: Failed to compile DS");
    }

    // Pixel Shader
    hr = D3DCompileFromFile(L"Shaders/Terrain.hlsl", nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE, "PS", "ps_5_1",
                            compileFlags, 0, &m_ps, &errors);
    if (FAILED(hr)) {
        if (errors) {
            OutputDebugStringA(reinterpret_cast<const char *>(errors->GetBufferPointer()));
        }
        throw std::runtime_error("TerrainRenderer: Failed to compile PS");
    }

    OutputDebugStringW(L"[INFO] Terrain shaders compiled successfully\n");
}

void TerrainRenderer::CreateRootSignature() {
    auto device = m_context->GetDevice();

    // ========================================================================
    // 根参数布局:
    //   slot 0: b0 cbPerObject (CBV) - Standard 模式
    //   slot 1: b1 cbPass (CBV)
    //   slot 2: b2 cbLights (CBV)
    //   slot 3: t0,space1 StructuredBuffer<MaterialData> (SRV)
    //   slot 4: t0 纹理 SRV（高度图、漫反射等）
    //   slot 5: t10 环境贴图 SRV
    //   slot 6: t11,space1 StructuredBuffer<DirShadowData> (SRV)
    //   slot 7: t14,space1 Texture2D 阴影贴图 (SRV)
    //   slot 8: t12,space1 StructuredBuffer<InstanceData> (SRV) - Instanced 模式
    // ========================================================================

    CD3DX12_ROOT_PARAMETER slotRootParameter[9];

    // 描述符范围
    CD3DX12_DESCRIPTOR_RANGE materialBufferRange;
    materialBufferRange.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 1, D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND);

    CD3DX12_DESCRIPTOR_RANGE texTable;
    texTable.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 8, 0, 0, D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND);

    CD3DX12_DESCRIPTOR_RANGE envMapTable;
    envMapTable.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 10, 0, D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND);

    CD3DX12_DESCRIPTOR_RANGE shadowDataTable;
    shadowDataTable.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 11, 1, D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND);

    CD3DX12_DESCRIPTOR_RANGE shadowMapTable;
    shadowMapTable.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 14, 1, D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND);

    CD3DX12_DESCRIPTOR_RANGE instanceDataRange;
    instanceDataRange.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 12, 1, D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND);

    // 根参数
    slotRootParameter[0].InitAsConstantBufferView(0, 0, D3D12_SHADER_VISIBILITY_ALL);
    slotRootParameter[1].InitAsConstantBufferView(1, 0, D3D12_SHADER_VISIBILITY_ALL);
    slotRootParameter[2].InitAsConstantBufferView(2, 0, D3D12_SHADER_VISIBILITY_ALL);
    slotRootParameter[3].InitAsDescriptorTable(1, &materialBufferRange, D3D12_SHADER_VISIBILITY_PIXEL);
    slotRootParameter[4].InitAsDescriptorTable(1, &texTable, D3D12_SHADER_VISIBILITY_ALL); // DS+PS 都需要地形纹理
    slotRootParameter[5].InitAsDescriptorTable(1, &envMapTable, D3D12_SHADER_VISIBILITY_PIXEL);
    slotRootParameter[6].InitAsDescriptorTable(1, &shadowDataTable, D3D12_SHADER_VISIBILITY_PIXEL);
    slotRootParameter[7].InitAsDescriptorTable(1, &shadowMapTable, D3D12_SHADER_VISIBILITY_PIXEL);
    slotRootParameter[8].InitAsDescriptorTable(1, &instanceDataRange, D3D12_SHADER_VISIBILITY_VERTEX);

    // 静态采样器（复用 OpaqueRenderer 的配置）
    CD3DX12_STATIC_SAMPLER_DESC staticSamplers[8];
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
    staticSamplers[7].Init(11, D3D12_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT, D3D12_TEXTURE_ADDRESS_MODE_BORDER,
                           D3D12_TEXTURE_ADDRESS_MODE_BORDER, D3D12_TEXTURE_ADDRESS_MODE_BORDER, 0.0f, 0,
                           D3D12_COMPARISON_FUNC_LESS_EQUAL, D3D12_STATIC_BORDER_COLOR_OPAQUE_BLACK, 0.0f, 0.0f,
                           D3D12_SHADER_VISIBILITY_PIXEL);

    CD3DX12_ROOT_SIGNATURE_DESC rootSigDesc(9, slotRootParameter, 8, staticSamplers,
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

void TerrainRenderer::CreatePSOs() {
    auto device = m_context->GetDevice();

    // 输入布局（与 GeometryGenerator::Vertex 一致）
    D3D12_INPUT_ELEMENT_DESC inputLayout[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"TANGENT", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 24, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 36, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0}};

    D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc = {};
    psoDesc.InputLayout = {inputLayout, _countof(inputLayout)};
    psoDesc.pRootSignature = m_rootSignature.Get();
    psoDesc.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
    psoDesc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
    psoDesc.SampleMask = UINT_MAX;
    psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_PATCH; // 关键：面片拓扑
    psoDesc.NumRenderTargets = 1;
    psoDesc.RTVFormats[0] = m_context->GetBackBufferFormat();
    psoDesc.DSVFormat = m_context->GetDepthStencilFormat();
    psoDesc.SampleDesc.Count = m_context->Is4xMsaaEnabled() ? 4 : 1;
    psoDesc.SampleDesc.Quality = m_context->Is4xMsaaEnabled() ? (m_context->Get4xMsaaQuality() - 1) : 0;

    // 深度模板状态
    D3D12_DEPTH_STENCIL_DESC depthStencilDesc = {};
    depthStencilDesc.DepthEnable = TRUE;
    depthStencilDesc.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    depthStencilDesc.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
    depthStencilDesc.StencilEnable = FALSE;
    psoDesc.DepthStencilState = depthStencilDesc;

    // 创建 Standard PSO（单物体）
    psoDesc.VS = {reinterpret_cast<BYTE *>(m_vsStandard->GetBufferPointer()), m_vsStandard->GetBufferSize()};
    psoDesc.HS = {reinterpret_cast<BYTE *>(m_hs->GetBufferPointer()), m_hs->GetBufferSize()};
    psoDesc.DS = {reinterpret_cast<BYTE *>(m_ds->GetBufferPointer()), m_ds->GetBufferSize()};
    psoDesc.PS = {reinterpret_cast<BYTE *>(m_ps->GetBufferPointer()), m_ps->GetBufferSize()};
    ThrowIfFailed(device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&m_psoStandard)));

    // 创建 Instanced PSO（实例化）
    psoDesc.VS = {reinterpret_cast<BYTE *>(m_vsInstanced->GetBufferPointer()), m_vsInstanced->GetBufferSize()};
    ThrowIfFailed(device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&m_psoInstanced)));

    OutputDebugStringW(L"[INFO] Terrain PSOs created successfully\n");
}

// ============================================================================
// 辅助方法
// ============================================================================

void TerrainRenderer::BindCommonResources(CommandList &cmdList, D3D12_GPU_VIRTUAL_ADDRESS passConstantsAddress,
                                          D3D12_GPU_VIRTUAL_ADDRESS lightCBAddress,
                                          D3D12_GPU_DESCRIPTOR_HANDLE materialBufferSRV) {
    cmdList.Get()->SetGraphicsRootSignature(m_rootSignature.Get());
    cmdList.Get()->SetGraphicsRootConstantBufferView(1, passConstantsAddress);
    cmdList.Get()->SetGraphicsRootConstantBufferView(2, lightCBAddress);

    if (materialBufferSRV.ptr != 0) {
        cmdList.Get()->SetGraphicsRootDescriptorTable(3, materialBufferSRV);
    }
}

} // namespace DX12Engine::Renderer