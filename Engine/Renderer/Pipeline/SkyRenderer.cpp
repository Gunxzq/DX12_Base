#include "SkyRenderer.h"
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

void SkyRenderer::SetDeviceContext(D3D12DeviceContext *context) { m_context = context; }

void SkyRenderer::Initialize() {
    if (!m_context) {
        throw std::runtime_error("SkyRenderer: Device context not set before Initialize");
    }

    LoadShaders();

    if (!m_vsBlob || !m_psBlob) {
        OutputDebugStringW(L"[ERROR] SkyRenderer: Failed to load shaders!\n");
        throw std::runtime_error("SkyRenderer: Failed to load shaders");
    }

    CreateRootSignature();
    CreatePSO();

    OutputDebugStringW(L"[INFO] SkyRenderer initialized successfully\n");
}

void SkyRenderer::OnResize(uint32_t width, uint32_t height) {
    // 天空盒不需要投影矩阵，直接使用相机的 ViewProj
    // 此方法保留用于接口一致性
}

void SkyRenderer::Update(float deltaTime) {
    // 天空盒不需要每帧更新逻辑
}

// ========================================================================
// 渲染辅助接口实现
// ========================================================================

void SkyRenderer::BeginFrame(CommandList &cmdList, D3D12_GPU_VIRTUAL_ADDRESS passConstantsAddress,
                             D3D12_GPU_DESCRIPTOR_HANDLE envMapSRV) {
    if (!m_pso || !m_rootSignature)
        return;

    cmdList.Get()->SetPipelineState(m_pso.Get());
    cmdList.Get()->SetGraphicsRootSignature(m_rootSignature.Get());
    cmdList.Get()->SetGraphicsRootConstantBufferView(1, passConstantsAddress);

    // 绑定环境贴图 SRV (slot 2, t10)
    if (envMapSRV.ptr != 0) {
        cmdList.Get()->SetGraphicsRootDescriptorTable(2, envMapSRV);
    }
}

void SkyRenderer::DrawSky(CommandList &cmdList, GeometryHandle skyboxGeometry,
                          D3D12_GPU_VIRTUAL_ADDRESS objectCBAddress) {
    if (!m_geometryManager) {
        OutputDebugStringW(L"[ERROR] SkyRenderer::DrawSky - GeometryResourceManager not set!\n");
        return;
    }

    const TriangleMesh *mesh = m_geometryManager->GetTriangleMesh(skyboxGeometry);
    if (!mesh || !mesh->isGpuReady) {
        return;
    }

    auto &gpuMgr = GpuResourceManager::GetInstance();
    ID3D12Resource *vbResource = gpuMgr.GetResource(mesh->vertexBufferHandle);
    ID3D12Resource *ibResource = gpuMgr.GetResource(mesh->indexBufferHandle);

    if (!vbResource || !ibResource) {
        OutputDebugStringW(L"[ERROR] SkyRenderer::DrawSky - Invalid vertex or index buffer!\n");
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

    cmdList.Get()->DrawIndexedInstanced(mesh->indexCount, 1, 0, 0, 0);
}

void SkyRenderer::EndFrame() {
    // 无状态需要重置
}

// ========================================================================
// 内部初始化
// ========================================================================

void SkyRenderer::LoadShaders() {
    UINT compileFlags = D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
    compileFlags |= D3DCOMPILE_ENABLE_UNBOUNDED_DESCRIPTOR_TABLES;

    Microsoft::WRL::ComPtr<ID3DBlob> errors = nullptr;
    HRESULT hr;

    hr = D3DCompileFromFile(L"Shaders/Sky.hlsl", nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE, "VS", "vs_5_1",
                            compileFlags, 0, &m_vsBlob, &errors);

    if (FAILED(hr)) {
        if (errors) {
            const char *errMsg = reinterpret_cast<const char *>(errors->GetBufferPointer());
            OutputDebugStringA("=== SKY VS COMPILATION ERROR ===\n");
            OutputDebugStringA(errMsg);
            OutputDebugStringA("================================\n");
        }
        throw std::runtime_error("SkyRenderer: Failed to compile Vertex Shader");
    }

    errors = nullptr;
    hr = D3DCompileFromFile(L"Shaders/Sky.hlsl", nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE, "PS", "ps_5_1",
                            compileFlags, 0, &m_psBlob, &errors);

    if (FAILED(hr)) {
        if (errors) {
            const char *errMsg = reinterpret_cast<const char *>(errors->GetBufferPointer());
            OutputDebugStringA("=== SKY PS COMPILATION ERROR ===\n");
            OutputDebugStringA(errMsg);
            OutputDebugStringA("================================\n");
        }
        throw std::runtime_error("SkyRenderer: Failed to compile Pixel Shader");
    }

    OutputDebugStringW(L"[INFO] Sky shaders compiled successfully\n");
}

void SkyRenderer::CreateRootSignature() {
    auto device = m_context->GetDevice();

    // ========================================================================
    // 天空盒根参数布局 (对齐 Sky.hlsl):
    //   slot 0: b0 cbPerObject      (CBV, 单位矩阵)
    //   slot 1: b1 cbPass           (CBV, 包含 gViewProj, gCameraPos)
    //   slot 2: t10                 环境贴图 SRV (描述符表)
    // ========================================================================
    CD3DX12_ROOT_PARAMETER slotRootParameter[3];

    CD3DX12_DESCRIPTOR_RANGE envMapTable;
    envMapTable.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 10, 0, D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND);

    slotRootParameter[0].InitAsConstantBufferView(0, 0, D3D12_SHADER_VISIBILITY_ALL);           // b0: cbPerObject
    slotRootParameter[1].InitAsConstantBufferView(1, 0, D3D12_SHADER_VISIBILITY_ALL);           // b1: cbPass
    slotRootParameter[2].InitAsDescriptorTable(1, &envMapTable, D3D12_SHADER_VISIBILITY_PIXEL); // t10

    // ========================================================================
    // 静态采样器: s2 (线性环绕) 和 s4 (各向异性环绕)
    // Common_PBR.hlsl 中 gSamplerAnisotropicWrap : register(s4)
    // ========================================================================
    CD3DX12_STATIC_SAMPLER_DESC staticSamplers[2];
    staticSamplers[0].Init(2, // register s2: gSamplerLinearWrap
                           D3D12_FILTER_MIN_MAG_MIP_LINEAR, D3D12_TEXTURE_ADDRESS_MODE_WRAP,
                           D3D12_TEXTURE_ADDRESS_MODE_WRAP, D3D12_TEXTURE_ADDRESS_MODE_WRAP);
    staticSamplers[1].Init(4, // register s4: gSamplerAnisotropicWrap
                           D3D12_FILTER_ANISOTROPIC, D3D12_TEXTURE_ADDRESS_MODE_WRAP,
                           D3D12_TEXTURE_ADDRESS_MODE_WRAP, D3D12_TEXTURE_ADDRESS_MODE_WRAP);

    CD3DX12_ROOT_SIGNATURE_DESC rootSigDesc(3, slotRootParameter, 2, staticSamplers,
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

void SkyRenderer::CreatePSO() {
    auto device = m_context->GetDevice();

    // 输入布局: 位置、法线、纹理坐标 (对齐 Sky.hlsl VertexIn)
    // GeometryGenerator::Vertex 布局: Position(12) + Normal(12) + TangentU(12) + TexC(8) = 44
    D3D12_INPUT_ELEMENT_DESC inputLayout[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 36, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0}};

    D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc = {};
    psoDesc.InputLayout = {inputLayout, _countof(inputLayout)};
    psoDesc.pRootSignature = m_rootSignature.Get();
    psoDesc.VS = {reinterpret_cast<BYTE *>(m_vsBlob->GetBufferPointer()), m_vsBlob->GetBufferSize()};
    psoDesc.PS = {reinterpret_cast<BYTE *>(m_psBlob->GetBufferPointer()), m_psBlob->GetBufferSize()};

    // 天空盒从内部看，剔除正面保留背面
    CD3DX12_RASTERIZER_DESC rasterDesc(D3D12_DEFAULT);
    rasterDesc.CullMode = D3D12_CULL_MODE_FRONT;
    psoDesc.RasterizerState = rasterDesc;
    psoDesc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);

    // 深度状态: 使用 LESS_EQUAL，允许深度等于远平面
    D3D12_DEPTH_STENCIL_DESC depthStencilDesc = {};
    depthStencilDesc.DepthEnable = TRUE;
    depthStencilDesc.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    depthStencilDesc.DepthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL; // 关键差异
    depthStencilDesc.StencilEnable = FALSE;
    psoDesc.DepthStencilState = depthStencilDesc;

    psoDesc.SampleMask = UINT_MAX;
    psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    psoDesc.NumRenderTargets = 1;
    psoDesc.RTVFormats[0] = m_context->GetBackBufferFormat();
    psoDesc.DSVFormat = m_context->GetDepthStencilFormat();
    psoDesc.SampleDesc.Count = m_context->Is4xMsaaEnabled() ? 4 : 1;
    psoDesc.SampleDesc.Quality = m_context->Is4xMsaaEnabled() ? (m_context->Get4xMsaaQuality() - 1) : 0;

    ThrowIfFailed(device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&m_pso)));

    OutputDebugStringW(L"[INFO] Sky PSO created successfully\n");
}

} // namespace DX12Engine::Renderer