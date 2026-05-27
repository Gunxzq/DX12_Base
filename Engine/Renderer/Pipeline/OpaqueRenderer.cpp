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

    LoadShaders();

    if (!m_vsBlob || !m_psBlob) {
        OutputDebugStringW(L"[ERROR] Failed to load shaders!\n");
        throw std::runtime_error("OpaqueRenderer: Failed to load shaders");
    }

    CreateRootSignature();
    CreatePSO();

    const auto &viewport = m_context->GetViewport();
    OnResize(static_cast<uint32_t>(viewport.Width), static_cast<uint32_t>(viewport.Height));

    OutputDebugStringW(L"[INFO] OpaqueRenderer initialized successfully\n");
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
// 渲染辅助接口实现
// ========================================================================

void OpaqueRenderer::BeginFrame(CommandList &cmdList, D3D12_GPU_VIRTUAL_ADDRESS passConstantsAddress,
                                D3D12_GPU_VIRTUAL_ADDRESS lightCBAddress) {
    if (!m_pso || !m_rootSignature)
        return;

    cmdList.Get()->SetPipelineState(m_pso.Get());
    cmdList.Get()->SetGraphicsRootSignature(m_rootSignature.Get());
    cmdList.Get()->SetGraphicsRootConstantBufferView(1, passConstantsAddress);
    cmdList.Get()->SetGraphicsRootConstantBufferView(3, lightCBAddress);
}

void OpaqueRenderer::DrawMesh(CommandList &cmdList, DX12Engine::Resource::GeometryHandle geometryHandle,
                              const DirectX::XMMATRIX &worldMatrix, D3D12_GPU_VIRTUAL_ADDRESS objectCBAddress,
                              D3D12_GPU_VIRTUAL_ADDRESS matCBAddress, D3D12_GPU_DESCRIPTOR_HANDLE textureSRV) {
    if (!m_geometryManager) {
        OutputDebugStringW(L"[ERROR] OpaqueRenderer::DrawMesh - GeometryResourceManager not set!\n");
        return;
    }

    // 1. 获取几何体数据
    const TriangleMesh *mesh = m_geometryManager->GetTriangleMesh(geometryHandle);

    if (!mesh || !mesh->isGpuReady) {
        return;
    }

    // 2. 获取 GPU 资源
    auto &gpuMgr = GpuResourceManager::GetInstance();
    ID3D12Resource *vbResource = gpuMgr.GetResource(mesh->vertexBufferHandle);
    ID3D12Resource *ibResource = gpuMgr.GetResource(mesh->indexBufferHandle);

    if (!vbResource || !ibResource) {
        OutputDebugStringW(L"[ERROR] OpaqueRenderer::DrawMesh - Invalid vertex or index buffer!\n");
        return;
    }

    // 3. 设置顶点/索引缓冲视图
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
    cmdList.Get()->SetGraphicsRootConstantBufferView(2, matCBAddress);

    cmdList.Get()->SetGraphicsRootDescriptorTable(4, textureSRV);

    cmdList.Get()->DrawIndexedInstanced(mesh->indexCount, 1, 0, 0, 0);
}

void OpaqueRenderer::EndFrame() {
    // 如果有需要每帧重置的状态，在此处处理
}

// ========================================================================
// 内部初始化
// ========================================================================

void OpaqueRenderer::LoadShaders() {

    UINT compileFlags = D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;

    Microsoft::WRL::ComPtr<ID3DBlob> errors = nullptr;
    HRESULT hr;

    hr = D3DCompileFromFile(L"Shaders/color.hlsl",             // 文件名
                            nullptr,                           // defines
                            D3D_COMPILE_STANDARD_FILE_INCLUDE, // includes
                            "VS",                              // entry point
                            "vs_5_1",                          // target profile
                            compileFlags,                      // flags1
                            0,                                 // flags2
                            &m_vsBlob,                         // output shader blob
                            &errors                            // error messages
    );

    if (FAILED(hr)) {
        if (errors) {
            OutputDebugStringA(reinterpret_cast<const char *>(errors->GetBufferPointer()));
        }
        throw std::runtime_error("OpaqueRenderer: Failed to compile Vertex Shader");
    }

    // 2. 编译像素着色器
    errors = nullptr; // 重置错误 Blob
    hr = D3DCompileFromFile(L"Shaders/color.hlsl", nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE, "PS", "ps_5_1",
                            compileFlags, 0, &m_psBlob, &errors);

    if (FAILED(hr)) {
        if (errors) {
            OutputDebugStringA(reinterpret_cast<const char *>(errors->GetBufferPointer()));
        }
        throw std::runtime_error("OpaqueRenderer: Failed to compile Pixel Shader");
    }

    OutputDebugStringW(L"[INFO] Shaders compiled successfully at runtime\n");
}

void OpaqueRenderer::CreateRootSignature() {
    auto device = m_context->GetDevice();

    CD3DX12_ROOT_PARAMETER slotRootParameter[5];

    // 只有纹理 SRV 范围
    CD3DX12_DESCRIPTOR_RANGE texTable;
    texTable.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND);

    slotRootParameter[0].InitAsConstantBufferView(0, 0, D3D12_SHADER_VISIBILITY_ALL);
    slotRootParameter[1].InitAsConstantBufferView(1, 0, D3D12_SHADER_VISIBILITY_ALL);
    slotRootParameter[2].InitAsConstantBufferView(2, 0, D3D12_SHADER_VISIBILITY_ALL);
    slotRootParameter[3].InitAsConstantBufferView(3, 0, D3D12_SHADER_VISIBILITY_ALL);
    slotRootParameter[4].InitAsDescriptorTable(1, &texTable, D3D12_SHADER_VISIBILITY_PIXEL);

    // 静态采样器
    CD3DX12_STATIC_SAMPLER_DESC staticSamplers[1];
    staticSamplers[0].Init(0, D3D12_FILTER_MIN_MAG_MIP_LINEAR);

    CD3DX12_ROOT_SIGNATURE_DESC rootSigDesc(5, slotRootParameter, 1, staticSamplers,
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

void OpaqueRenderer::CreatePSO() {
    auto device = m_context->GetDevice();

    D3D12_INPUT_ELEMENT_DESC inputLayout[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 28, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 40, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0} // 添加
    };
    D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc = {};
    psoDesc.InputLayout = {inputLayout, _countof(inputLayout)};
    psoDesc.pRootSignature = m_rootSignature.Get();
    psoDesc.VS = {reinterpret_cast<BYTE *>(m_vsBlob->GetBufferPointer()), m_vsBlob->GetBufferSize()};
    psoDesc.PS = {reinterpret_cast<BYTE *>(m_psBlob->GetBufferPointer()), m_psBlob->GetBufferSize()};

    psoDesc.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);

    // // // 启用线框模式以便调试
    // D3D12_RASTERIZER_DESC rasterizerDesc = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
    // rasterizerDesc.FillMode = D3D12_FILL_MODE_WIREFRAME;
    // rasterizerDesc.CullMode = D3D12_CULL_MODE_NONE; // 禁用背面剔除
    // psoDesc.RasterizerState = rasterizerDesc;

    psoDesc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);

    psoDesc.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);

    // 临时
    D3D12_DEPTH_STENCIL_DESC depthStencilDesc = {};
    depthStencilDesc.DepthEnable = FALSE;
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

    OutputDebugStringW(L"[INFO] PSO created successfully\n");
}

} // namespace DX12Engine::Renderer