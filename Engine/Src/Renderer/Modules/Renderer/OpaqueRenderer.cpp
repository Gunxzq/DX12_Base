#include "Renderer/Modules/Renderer/OpaqueRenderer.h"
#include "Common/d3dUtil.h"
#include "Renderer/Core/D3D12DeviceContext.h"
#include "Renderer/Utils/GeometryGenerator.h"
#include "System/ECS/Components.h"
#include "System/Resource/GpuResourceManager.h"
#include <DirectXMath.h>
#include <entt/entt.hpp>

using namespace DirectX;
using namespace DX12Engine::ECS;
using namespace DX12Engine::Renderer;
using namespace DX12Engine::System::Resource;

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

    auto device = m_context->GetDevice();
    UINT cbSize = d3dUtil::CalcConstantBufferByteSize(sizeof(XMFLOAT4X4));

    for (uint32_t i = 0; i < 3; ++i) {
        ThrowIfFailed(d3dUtil::CreateUploadBuffer(device, cbSize, D3D12_RESOURCE_STATE_GENERIC_READ,
                                                  &m_frameResources[i].constantBuffer));

        m_frameResources[i].constantBuffer->Map(0, nullptr, &m_frameResources[i].mappedData);
    }

    OnResize(1280, 720);

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

void OpaqueRenderer::BeginFrame(CommandList &cmdList, uint32_t backBufferIndex) {
    if (!m_pso || !m_rootSignature)
        return;

    cmdList.Get()->SetPipelineState(m_pso.Get());
    cmdList.Get()->SetGraphicsRootSignature(m_rootSignature.Get());

    UpdateConstantBuffer(backBufferIndex);
    cmdList.Get()->SetGraphicsRootConstantBufferView(
        0, m_frameResources[backBufferIndex].constantBuffer->GetGPUVirtualAddress());

    auto rtvHandle = m_context->GetCurrentBackBufferView();
    auto dsvHandle = m_context->GetDepthStencilView();

    if (dsvHandle.ptr == 0) {
        OutputDebugStringW(L"[ERROR] DSV handle is null!\n");
    }

    cmdList.Get()->OMSetRenderTargets(1, &rtvHandle, true, &dsvHandle);
}

void OpaqueRenderer::DrawMesh(CommandList &cmdList, const MeshComponent &mesh, const TransformComponent &transform,
                              uint32_t backBufferIndex) {
    // 1. 获取 GPU 资源指针
    auto &gpuMgr = System::Resource::GpuResourceManager::GetInstance();

    ID3D12Resource *vb = gpuMgr.GetResource(mesh.vertexBuffer);
    ID3D12Resource *ib = gpuMgr.GetResource(mesh.indexBuffer);

    if (!vb || !ib) {
        OutputDebugStringW(L"[ERROR] OpaqueRenderer::DrawMesh - Invalid vertex or index buffer!\n");
        return;
    }

    // 2. 使用 MeshComponent 中存储的 VBV/IBV（包含正确的 stride）
    cmdList.Get()->IASetVertexBuffers(0, 1, &mesh.vertexBufferView);
    cmdList.Get()->IASetIndexBuffer(&mesh.indexBufferView);
    cmdList.Get()->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    // 3. 更新常量缓冲区中的世界矩阵
    UpdateConstantBufferWithTransform(backBufferIndex, transform);

    // 4. 绘制调用
    static bool firstDraw = true;
    if (firstDraw) {
        wchar_t debugInfo[256];
        swprintf_s(debugInfo,
                   L"[DEBUG] Drawing mesh: vertices=%u, indices=%u, stride=%u, position=(%.1f, %.1f, %.1f)\n",
                   mesh.vertexCount, mesh.indexCount, mesh.vertexBufferView.StrideInBytes, transform.position.x,
                   transform.position.y, transform.position.z);
        OutputDebugStringW(debugInfo);
        firstDraw = false;
    }

    cmdList.Get()->DrawIndexedInstanced(mesh.indexCount, 1, 0, 0, 0);
}

void OpaqueRenderer::EndFrame() {
    // 如果有需要每帧重置的状态，在此处处理
}

// ========================================================================
// 内部初始化
// ========================================================================

void OpaqueRenderer::LoadShaders() {
    m_vsBlob = d3dUtil::LoadBinary(L"Shaders/color_vs.cso");
    m_psBlob = d3dUtil::LoadBinary(L"Shaders/color_ps.cso");
}

void OpaqueRenderer::CreateRootSignature() {
    auto device = m_context->GetDevice();

    CD3DX12_ROOT_PARAMETER slotRootParameter[1];
    slotRootParameter[0].InitAsConstantBufferView(0);

    CD3DX12_ROOT_SIGNATURE_DESC rootSigDesc(1, slotRootParameter, 0, nullptr,
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
        {"COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0}};

    D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc = {};
    psoDesc.InputLayout = {inputLayout, _countof(inputLayout)};
    psoDesc.pRootSignature = m_rootSignature.Get();
    psoDesc.VS = {reinterpret_cast<BYTE *>(m_vsBlob->GetBufferPointer()), m_vsBlob->GetBufferSize()};
    psoDesc.PS = {reinterpret_cast<BYTE *>(m_psBlob->GetBufferPointer()), m_psBlob->GetBufferSize()};

    psoDesc.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);

    // // 启用线框模式以便调试
    // D3D12_RASTERIZER_DESC rasterizerDesc = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
    // rasterizerDesc.FillMode = D3D12_FILL_MODE_WIREFRAME;
    // rasterizerDesc.CullMode = D3D12_CULL_MODE_NONE; // 禁用背面剔除
    // psoDesc.RasterizerState = rasterizerDesc;

    psoDesc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);

    psoDesc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);

    // psoDesc.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);

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

// ========================================================================
// 辅助方法
// ========================================================================

void OpaqueRenderer::UpdateConstantBuffer(uint32_t backBufferIndex) {
    XMMATRIX world = XMMatrixIdentity();
    XMMATRIX view = XMMatrixLookAtLH(XMVectorSet(0.0f, 2.0f, -5.0f, 1.0f), XMVectorSet(0.0f, 0.0f, 0.0f, 1.0f),
                                     XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f));

    XMMATRIX worldViewProj = world * view * m_projectionMatrix;
    XMFLOAT4X4 worldViewProjFX;
    XMStoreFloat4x4(&worldViewProjFX, XMMatrixTranspose(worldViewProj));

    memcpy(m_frameResources[backBufferIndex].mappedData, &worldViewProjFX, sizeof(XMFLOAT4X4));
}

void OpaqueRenderer::UpdateConstantBufferWithTransform(uint32_t backBufferIndex, const TransformComponent &transform) {
    // 从 TransformComponent 构建世界矩阵
    XMMATRIX translation = XMMatrixTranslation(transform.position.x, transform.position.y, transform.position.z);

    XMMATRIX rotationX = XMMatrixRotationX(transform.rotation.x);
    XMMATRIX rotationY = XMMatrixRotationY(transform.rotation.y);
    XMMATRIX rotationZ = XMMatrixRotationZ(transform.rotation.z);
    XMMATRIX rotation = rotationX * rotationY * rotationZ;

    XMMATRIX scale = XMMatrixScaling(transform.scale.x, transform.scale.y, transform.scale.z);

    XMMATRIX world = scale * rotation * translation;

    // 视图矩阵（相机位置）
    XMMATRIX view = XMMatrixLookAtLH(XMVectorSet(0.0f, 2.0f, -10.0f, 1.0f), // 相机位置：往后退到 -10
                                     XMVectorSet(0.0f, 0.0f, 0.0f, 1.0f),   // 观察目标：原点
                                     XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f)    // 上方向
    );

    XMMATRIX worldViewProj = world * view * m_projectionMatrix;
    XMFLOAT4X4 worldViewProjFX;
    XMStoreFloat4x4(&worldViewProjFX, XMMatrixTranspose(worldViewProj));

    memcpy(m_frameResources[backBufferIndex].mappedData, &worldViewProjFX, sizeof(XMFLOAT4X4));
}

} // namespace DX12Engine::Renderer