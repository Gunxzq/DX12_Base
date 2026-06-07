// BillboardRenderer.cpp
#include "BillboardRenderer.h"
#include "Common/d3dUtil.h"
#include "Renderer/RHI/D3D12DeviceContext.h"
#include <d3dcompiler.h>

using namespace DirectX;
using namespace DX12Engine::Resource;

namespace DX12Engine::Renderer {

// ============================================================================
// 生命周期管理
// ============================================================================

void BillboardRenderer::SetDeviceContext(D3D12DeviceContext *context) { m_context = context; }

void BillboardRenderer::Initialize() {
    if (!m_context) {
        throw std::runtime_error("BillboardRenderer: Device context not set before Initialize");
    }

    LoadShaders();
    CreateRootSignature();
    CreatePSO();

    OutputDebugStringW(L"[INFO] BillboardRenderer initialized successfully\n");
}

void BillboardRenderer::OnResize(uint32_t width, uint32_t height) {
    (void)width;
    (void)height;
}

void BillboardRenderer::Update(float deltaTime) { (void)deltaTime; }

void BillboardRenderer::EndFrame() {
    // 无每帧清理需求
}

// ============================================================================
// 渲染接口
// ============================================================================

/**
 * @brief 开始渲染帧
 * @param cmdList 命令列表
 * @param passConstantsAddress 传递常量缓冲区地址
 * @param lightCBAddress 光线缓冲区地址
 * @param materialBufferSRV 材料缓冲区SRV
 * @param billboardTextureSRV 公告牌 Texture2DArray SRV
 * @date 2026-06-05
 */
void BillboardRenderer::BeginFrame(CommandList &cmdList, D3D12_GPU_VIRTUAL_ADDRESS passConstantsAddress,
                                   D3D12_GPU_VIRTUAL_ADDRESS lightCBAddress,
                                   D3D12_GPU_DESCRIPTOR_HANDLE materialBufferSRV,
                                   D3D12_GPU_DESCRIPTOR_HANDLE billboardTextureSRV) {
    if (!m_pso || !m_rootSignature) {
        OutputDebugStringW(L"[ERROR] BillboardRenderer::BeginFrame: PSO or RootSignature not initialized\n");
        return;
    }

    cmdList.Get()->SetGraphicsRootSignature(m_rootSignature.Get());
    cmdList.Get()->SetGraphicsRootConstantBufferView(1, passConstantsAddress);
    cmdList.Get()->SetGraphicsRootConstantBufferView(2, lightCBAddress);

    if (materialBufferSRV.ptr != 0) {
        cmdList.Get()->SetGraphicsRootDescriptorTable(3, materialBufferSRV);
    }

    if (billboardTextureSRV.ptr != 0) {
        cmdList.Get()->SetGraphicsRootDescriptorTable(4, billboardTextureSRV);
    }
}

void BillboardRenderer::DrawBillboard(CommandList &cmdList, const BillboardRenderItem &item) {
    if (!item.IsValid())
        return;

    cmdList.Get()->SetPipelineState(m_pso.Get());

    // slot 8: 实例数据（直接 SRV，与 OpaqueRenderer instanced 路径一致）
    cmdList.Get()->SetGraphicsRootShaderResourceView(8, item.instanceBufferAddress);

    // PSO 使用 POINT 拓扑，需要显式设置
    cmdList.Get()->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_POINTLIST);

    // 每个实例一个点，GS 扩展为四边形
    cmdList.Get()->DrawInstanced(1, item.instanceCount, 0, 0);
}

// ============================================================================
// PSO 切换
// ============================================================================

void BillboardRenderer::SetPSO(CommandList &cmdList) const {
    if (m_pso) {
        cmdList.Get()->SetPipelineState(m_pso.Get());
    }
}

// ============================================================================
// 内部初始化
// ============================================================================

void BillboardRenderer::LoadShaders() {
    UINT compileFlags = D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
    compileFlags |= D3DCOMPILE_ENABLE_UNBOUNDED_DESCRIPTOR_TABLES;
    Microsoft::WRL::ComPtr<ID3DBlob> errors = nullptr;
    HRESULT hr;

    // 顶点着色器
    hr = D3DCompileFromFile(L"Shaders/Billboard.hlsl", nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE, "VS", "vs_5_1",
                            compileFlags, 0, &m_vs, &errors);
    if (FAILED(hr)) {
        if (errors) {
            OutputDebugStringA(reinterpret_cast<const char *>(errors->GetBufferPointer()));
        }
        throw std::runtime_error("BillboardRenderer: Failed to compile VS");
    }

    // 几何着色器
    hr = D3DCompileFromFile(L"Shaders/Billboard.hlsl", nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE, "GS", "gs_5_1",
                            compileFlags, 0, &m_gs, &errors);
    if (FAILED(hr)) {
        if (errors) {
            OutputDebugStringA(reinterpret_cast<const char *>(errors->GetBufferPointer()));
        }
        throw std::runtime_error("BillboardRenderer: Failed to compile GS");
    }

    // 像素着色器
    hr = D3DCompileFromFile(L"Shaders/Billboard.hlsl", nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE, "PS", "ps_5_1",
                            compileFlags, 0, &m_ps, &errors);
    if (FAILED(hr)) {
        if (errors) {
            OutputDebugStringA(reinterpret_cast<const char *>(errors->GetBufferPointer()));
        }
        throw std::runtime_error("BillboardRenderer: Failed to compile PS");
    }

    OutputDebugStringW(L"[INFO] Billboard shaders compiled successfully\n");
}

void BillboardRenderer::CreateRootSignature() {
    auto device = m_context->GetDevice();

    // ========================================================================
    // 根参数布局（与 OpaqueRenderer 保持一致，便于统一管理）:
    //   slot 0: b0 cbPerObject      (CBV) - Billboard 不使用但仍占位对齐
    //   slot 1: b1 cbPass           (CBV)
    //   slot 2: b2 cbLights         (CBV)
    //   slot 3: t0,space1           StructuredBuffer<MaterialData> (SRV)
    //   slot 4: t20                 Texture2DArray SRV（公告牌专用）
    //   slot 5-7: 未使用（占位对齐）
    //   slot 8: t12,space1          StructuredBuffer<BillboardInstanceData> (SRV)
    // ========================================================================

    CD3DX12_ROOT_PARAMETER slotRootParameter[9];

    CD3DX12_DESCRIPTOR_RANGE materialBufferRange;
    materialBufferRange.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 1, D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND);

    CD3DX12_DESCRIPTOR_RANGE billboardTexRange;
    billboardTexRange.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 20, 0,
                           D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND); // 单个 Texture2DArray SRV at t20

    // slot 0: 占位 cbPerObject (Billboard VS 中未使用，但 cbPerObject 在 cbuffer 中仍有定义)
    slotRootParameter[0].InitAsConstantBufferView(0, 0, D3D12_SHADER_VISIBILITY_ALL);
    slotRootParameter[1].InitAsConstantBufferView(1, 0, D3D12_SHADER_VISIBILITY_ALL);
    slotRootParameter[2].InitAsConstantBufferView(2, 0, D3D12_SHADER_VISIBILITY_ALL);
    slotRootParameter[3].InitAsDescriptorTable(1, &materialBufferRange, D3D12_SHADER_VISIBILITY_PIXEL);
    slotRootParameter[4].InitAsDescriptorTable(1, &billboardTexRange, D3D12_SHADER_VISIBILITY_PIXEL);
    // slot 5, 6, 7: 占位对齐（绑定到 shader 中不存在的寄存器，避免冲突）
    slotRootParameter[5].InitAsConstantBufferView(5, 0, D3D12_SHADER_VISIBILITY_ALL);
    slotRootParameter[6].InitAsConstantBufferView(6, 0, D3D12_SHADER_VISIBILITY_ALL);
    slotRootParameter[7].InitAsConstantBufferView(7, 0, D3D12_SHADER_VISIBILITY_ALL);
    // slot 8: t12,space1 — VS 和 PS 都需要访问（PS 读取 MaterialIndex）
    slotRootParameter[8].InitAsShaderResourceView(12, 1, D3D12_SHADER_VISIBILITY_ALL);

    // 静态采样器（只需要线性采样器）
    CD3DX12_STATIC_SAMPLER_DESC staticSamplers[1];
    staticSamplers[0].Init(2, D3D12_FILTER_MIN_MAG_MIP_LINEAR, D3D12_TEXTURE_ADDRESS_MODE_WRAP,
                           D3D12_TEXTURE_ADDRESS_MODE_WRAP, D3D12_TEXTURE_ADDRESS_MODE_WRAP);

    CD3DX12_ROOT_SIGNATURE_DESC rootSigDesc(9, slotRootParameter, 1, staticSamplers, D3D12_ROOT_SIGNATURE_FLAG_NONE);

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

void BillboardRenderer::CreatePSO() {
    auto device = m_context->GetDevice();

    // 深度模板状态：写入深度（与龙书一致，D3D12_DEFAULT）
    D3D12_DEPTH_STENCIL_DESC depthStencilDesc = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);

    // 光栅化状态：双面渲染（公告牌背面也可能可见）
    D3D12_RASTERIZER_DESC rasterizerDesc = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
    rasterizerDesc.CullMode = D3D12_CULL_MODE_NONE;
    rasterizerDesc.DepthClipEnable = TRUE;

    // 混合状态：不透明（通过 clip() 做 Alpha 裁剪，无需混合）
    D3D12_BLEND_DESC blendDesc = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
    blendDesc.RenderTarget[0].BlendEnable = FALSE;

    D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc = {};
    psoDesc.InputLayout = {nullptr, 0}; // 无顶点输入——所有数据从 StructuredBuffer 读取
    psoDesc.pRootSignature = m_rootSignature.Get();
    psoDesc.VS = {reinterpret_cast<BYTE *>(m_vs->GetBufferPointer()), m_vs->GetBufferSize()};
    psoDesc.GS = {reinterpret_cast<BYTE *>(m_gs->GetBufferPointer()), m_gs->GetBufferSize()};
    psoDesc.PS = {reinterpret_cast<BYTE *>(m_ps->GetBufferPointer()), m_ps->GetBufferSize()};
    psoDesc.RasterizerState = rasterizerDesc;
    psoDesc.BlendState = blendDesc;
    psoDesc.DepthStencilState = depthStencilDesc;
    psoDesc.SampleMask = UINT_MAX;
    psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT;
    psoDesc.NumRenderTargets = 1;
    psoDesc.RTVFormats[0] = m_context->GetBackBufferFormat();
    psoDesc.DSVFormat = m_context->GetDepthStencilFormat();
    psoDesc.SampleDesc.Count = m_context->Is4xMsaaEnabled() ? 4 : 1;
    psoDesc.SampleDesc.Quality = m_context->Is4xMsaaEnabled() ? (m_context->Get4xMsaaQuality() - 1) : 0;

    ThrowIfFailed(device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&m_pso)));

    OutputDebugStringW(L"[INFO] Billboard PSO created successfully\n");
}

} // namespace DX12Engine::Renderer