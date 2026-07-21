#include "GridRenderer.h"
#include "GridManager.h"
#include "Renderer/RHI/D3D12DeviceContext.h"
#include "Resource/GpuResourceManager.h"

namespace DX12Engine::Renderer {

// ========================================================================
// IRenderer 接口实现
// ========================================================================

void GridRenderer::SetDeviceContext(D3D12DeviceContext *context) {
    m_context = context;
}

void GridRenderer::Initialize() {
    if (m_initialized || !m_context)
        return;

    ID3D12Device *device = m_context->GetDevice();
    DXGI_FORMAT depthFormat = m_context->GetDepthStencilFormat();

    m_vsBlob = d3dUtil::CompileShader(L"Shaders/grid.hlsl", nullptr, "VS", "vs_5_1");
    if (!m_vsBlob)
        return;
    m_psBlob = d3dUtil::CompileShader(L"Shaders/grid.hlsl", nullptr, "PS", "ps_5_1");
    if (!m_psBlob)
        return;

    CreateRootSignature();
    CreatePSO(device, depthFormat);

    m_initialized = true;
}

void GridRenderer::OnResize(uint32_t width, uint32_t height) {
    // GridRenderer 不依赖视口尺寸
}

void GridRenderer::EndFrame() {
    // 无每帧结束操作
}

void GridRenderer::Update(float deltaTime) {
    // 无每帧更新
}

// ========================================================================
// 内部初始化
// ========================================================================

void GridRenderer::CreateRootSignature() {
    ID3D12Device *device = m_context->GetDevice();

    // 根参数：b0 = GridCB (viewProj + cameraPos + gridParams + snapOffset)
    CD3DX12_ROOT_PARAMETER rootParam = {};
    rootParam.ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    rootParam.Descriptor.ShaderRegister = 0;
    rootParam.Descriptor.RegisterSpace = 0;
    rootParam.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    CD3DX12_ROOT_SIGNATURE_DESC rootSigDesc = {};
    rootSigDesc.Init(1, &rootParam, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT);

    Microsoft::WRL::ComPtr<ID3DBlob> sigBlob;
    HRESULT hr = D3D12SerializeRootSignature(&rootSigDesc, D3D_ROOT_SIGNATURE_VERSION_1, &sigBlob, nullptr);
    if (FAILED(hr))
        return;
    hr = device->CreateRootSignature(0, sigBlob->GetBufferPointer(), sigBlob->GetBufferSize(),
                                     IID_PPV_ARGS(&m_rootSig));
    if (FAILED(hr))
        return;
}

void GridRenderer::CreatePSO(ID3D12Device *device, DXGI_FORMAT depthFormat) {
    D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc = {};
    psoDesc.pRootSignature = m_rootSig.Get();
    psoDesc.VS = {m_vsBlob->GetBufferPointer(), m_vsBlob->GetBufferSize()};
    psoDesc.PS = {m_psBlob->GetBufferPointer(), m_psBlob->GetBufferSize()};
    psoDesc.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
    psoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    // 混合状态 - 颜色通道用 SRC_ALPHA 混合（网格与背景融合）
    // 但 alpha 通道保留目标值（1.0），避免 ImGui 显示时出现半透明
    D3D12_BLEND_DESC blendDesc = {};
    blendDesc.RenderTarget[0].BlendEnable = TRUE;
    blendDesc.RenderTarget[0].SrcBlend = D3D12_BLEND_SRC_ALPHA;
    blendDesc.RenderTarget[0].DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
    blendDesc.RenderTarget[0].BlendOp = D3D12_BLEND_OP_ADD;
    blendDesc.RenderTarget[0].SrcBlendAlpha = D3D12_BLEND_ZERO;
    blendDesc.RenderTarget[0].DestBlendAlpha = D3D12_BLEND_ONE;
    blendDesc.RenderTarget[0].BlendOpAlpha = D3D12_BLEND_OP_ADD;
    blendDesc.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    psoDesc.BlendState = blendDesc;
    psoDesc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    psoDesc.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
    psoDesc.DepthStencilState.DepthEnable = FALSE;
    psoDesc.DepthStencilState.StencilEnable = FALSE;
    psoDesc.SampleMask = UINT_MAX;
    psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    psoDesc.NumRenderTargets = 1;
    psoDesc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    psoDesc.DSVFormat = depthFormat;
    psoDesc.SampleDesc.Count = 1;
    D3D12_INPUT_ELEMENT_DESC inputLayout[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0}};
    psoDesc.InputLayout.NumElements = 1;
    psoDesc.InputLayout.pInputElementDescs = inputLayout;

    HRESULT hr = device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&m_pso));
    if (FAILED(hr))
        return;
}

void GridRenderer::Shutdown() {
    if (!m_initialized)
        return;
    m_pso.Reset();
    m_rootSig.Reset();
    m_vsBlob.Reset();
    m_psBlob.Reset();
    m_initialized = false;
}

// ========================================================================
// 绘制
// ========================================================================

void GridRenderer::Draw(ID3D12GraphicsCommandList *cmdList, const DirectX::XMMATRIX &viewProj,
                         const DirectX::XMFLOAT3 &cameraPos) {
    if (!m_initialized)
        return;

    auto &gridMgr = GridManager::GetInstance();
    if (!gridMgr.IsVisible())
        return;

    auto &gpuMgr = Resource::GpuResourceManager::GetInstance();

    D3D12_GPU_VIRTUAL_ADDRESS cbAddress = gridMgr.GetGridCBAddress();
    Resource::GpuResourceHandle vbHandle = gridMgr.GetQuadVBHandle();
    Resource::GpuResourceHandle ibHandle = gridMgr.GetQuadIBHandle();

    ID3D12Resource *vbRes = gpuMgr.GetResource(vbHandle);
    ID3D12Resource *ibRes = gpuMgr.GetResource(ibHandle);
    if (!cbAddress || !vbRes || !ibRes)
        return;

    // ── 设置 PSO + 根签名 ──
    cmdList->SetPipelineState(m_pso.Get());
    cmdList->SetGraphicsRootSignature(m_rootSig.Get());
    cmdList->SetGraphicsRootConstantBufferView(0, cbAddress);

    // ── VB ──
    D3D12_VERTEX_BUFFER_VIEW vbView = {};
    vbView.BufferLocation = vbRes->GetGPUVirtualAddress();
    vbView.SizeInBytes = 4 * sizeof(float) * 3;
    vbView.StrideInBytes = sizeof(float) * 3;
    cmdList->IASetVertexBuffers(0, 1, &vbView);

    // ── IB ──
    D3D12_INDEX_BUFFER_VIEW ibView = {};
    ibView.BufferLocation = ibRes->GetGPUVirtualAddress();
    ibView.SizeInBytes = 6 * sizeof(uint16_t);
    ibView.Format = DXGI_FORMAT_R16_UINT;
    cmdList->IASetIndexBuffer(&ibView);

    cmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    cmdList->DrawIndexedInstanced(6, 1, 0, 0, 0);
}

} // namespace DX12Engine::Renderer