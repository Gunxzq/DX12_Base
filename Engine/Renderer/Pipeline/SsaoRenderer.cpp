#include "SsaoRenderer.h"
#include "Renderer/RHI/Command/CommandList/CommandList.h"
#include "Renderer/RHI/D3D12DeviceContext.h"
#include "Resource/Core/DescriptorHeapCollection.h"
#include <d3dx12.h>

using namespace DX12Engine::Resource;
using namespace DirectX;

namespace DX12Engine::Renderer {

// ========================================================================
// 常量
// ========================================================================
static constexpr uint32_t kRandomTexSize = 4;
static constexpr uint32_t kSampleCount = 14;

// SSAO 常量缓冲布局（匹配 Ssao.hlsl cbuffer cbSsao）
struct alignas(256) SsaoCBData {
    DirectX::XMFLOAT4X4 View;            // 64 — 视图矩阵（法线 World→View）
    DirectX::XMFLOAT4X4 Proj;            // 64
    DirectX::XMFLOAT4X4 InvProj;         // 64
    DirectX::XMFLOAT4X4 ProjTex;         // 64
    DirectX::XMFLOAT4 OffsetVectors[14]; // 224
    // 以下 4 个 float 共占 16 字节（1 个 float4 register）
    float OcclusionRadius;    // 4
    float OcclusionFadeStart; // 4
    float OcclusionFadeEnd;   // 4
    float SurfaceEpsilon;     // 4
    // pad 到 16 字节倍数（总计 448 = 28 × float4）
    float Pad[4];
};

// 14 个半球偏移向量（均匀分布）
static const DirectX::XMFLOAT4 kOffsetVectors[14] = {
    {0.538f, 0.463f, -0.351f, 0.0f},   {0.287f, -0.162f, -0.956f, 0.0f},  {0.315f, 0.123f, -0.943f, 0.0f},
    {-0.697f, -0.176f, -0.712f, 0.0f}, {0.013f, 0.969f, 0.247f, 0.0f},    {0.602f, 0.403f, -0.694f, 0.0f},
    {-0.367f, 0.384f, 0.846f, 0.0f},   {-0.800f, 0.033f, -0.618f, 0.0f},  {0.962f, 0.149f, 0.245f, 0.0f},
    {-0.416f, -0.251f, -0.869f, 0.0f}, {-0.482f, -0.604f, 0.639f, 0.0f},  {0.002f, 0.368f, 0.934f, 0.0f},
    {-0.567f, 0.018f, -0.829f, 0.0f},  {-0.114f, -0.927f, -0.363f, 0.0f},
};

// ========================================================================
// 生命周期
// ========================================================================

void SsaoRenderer::SetDeviceContext(D3D12DeviceContext *context) { m_deviceContext = context; }

void SsaoRenderer::SetDescriptorHeaps(DX12Engine::Resource::DescriptorHeapCollection *heaps) {
    m_descriptorHeaps = heaps;
}

void SsaoRenderer::Initialize() {
    if (m_initialized)
        return;
    if (!m_deviceContext || !m_descriptorHeaps)
        return;

    auto *device = m_deviceContext->GetDevice();
    if (!device)
        return;

    const auto &vp = m_deviceContext->GetViewport();
    m_width = static_cast<uint32_t>(vp.Width);
    m_height = static_cast<uint32_t>(vp.Height);
    if (m_width == 0 || m_height == 0)
        return;

    // 根签名
    BuildRootSignatures();

    // SSAO + 模糊 PSO
    CreatePipelines();

    // SSAO 常量缓冲（UPLOAD 堆，8 字节对齐补丁到 256）
    {
        CD3DX12_HEAP_PROPERTIES heapProps(D3D12_HEAP_TYPE_UPLOAD);
        auto bufDesc = CD3DX12_RESOURCE_DESC::Buffer(sizeof(SsaoCBData));
        device->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &bufDesc, D3D12_RESOURCE_STATE_GENERIC_READ,
                                        nullptr, IID_PPV_ARGS(&m_ssaoCB));
        if (m_ssaoCB) {
            m_ssaoCB->SetName(L"SsaoCB");
            m_ssaoCB->Map(0, nullptr, &m_ssaoCBMapped);
        }
    }

    m_initialized = true;
}

void SsaoRenderer::Shutdown() {
    if (!m_initialized)
        return;

    m_ssaoPSO.Reset();
    m_blurPSO.Reset();
    m_randomVectorMapSRV = {};

    if (m_ssaoCBMapped && m_ssaoCB) {
        m_ssaoCB->Unmap(0, nullptr);
        m_ssaoCBMapped = nullptr;
    }
    m_ssaoCB.Reset();
    m_aoRootSig.Reset();
    m_blurRootSig.Reset();
    m_randomVectorMapSRV = {};

    m_deviceContext = nullptr;
    m_descriptorHeaps = nullptr;
    m_width = 0;
    m_height = 0;
    m_initialized = false;
}

// ========================================================================
// 每帧执行
// ========================================================================

void SsaoRenderer::Execute(CommandList &cmdList, ID3D12PipelineState *aoPSO, ID3D12PipelineState *blurPSO,
                           D3D12_GPU_DESCRIPTOR_HANDLE depthSRV, D3D12_GPU_DESCRIPTOR_HANDLE normalSRV,
                           D3D12_GPU_DESCRIPTOR_HANDLE ambientSRV, D3D12_CPU_DESCRIPTOR_HANDLE ambientRTV,
                           D3D12_GPU_DESCRIPTOR_HANDLE ambient1SRV, D3D12_CPU_DESCRIPTOR_HANDLE ambient1RTV,
                           ID3D12Resource *ambientRes0, ID3D12Resource *ambientRes1, const DirectX::XMFLOAT4X4 &view,
                           const DirectX::XMFLOAT4X4 &proj) {
    if (!m_initialized)
        return;

    auto *native = cmdList.Get();

    // 更新 SSAO 常量缓冲
    if (m_ssaoCBMapped) {
        auto *cb = static_cast<SsaoCBData *>(m_ssaoCBMapped);

        XMStoreFloat4x4(&cb->View, XMLoadFloat4x4(&view));
        XMStoreFloat4x4(&cb->Proj, XMLoadFloat4x4(&proj));
        XMStoreFloat4x4(&cb->InvProj, XMMatrixInverse(nullptr, XMLoadFloat4x4(&proj)));
        XMMATRIX texTransform = XMMatrixScaling(0.5f, -0.5f, 1.0f) * XMMatrixTranslation(0.5f, 0.5f, 0.0f);
        XMStoreFloat4x4(&cb->ProjTex, XMLoadFloat4x4(&proj) * texTransform);
        memcpy(cb->OffsetVectors, kOffsetVectors, sizeof(kOffsetVectors));
        cb->OcclusionRadius = 0.6f;    // 略微增大（原 0.5）
        cb->OcclusionFadeStart = 0.3f; // 适中衰减起始
        cb->OcclusionFadeEnd = 2.5f;   // 适度延长
        cb->SurfaceEpsilon = 0.05f;    // 恢复标准容差
        cb->Pad[0] = cb->Pad[1] = cb->Pad[2] = cb->Pad[3] = 0.0f;
    }

    auto barrier = [&](ID3D12Resource *res, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to) {
        if (!res)
            return;
        auto b = CD3DX12_RESOURCE_BARRIER::Transition(res, from, to);
        native->ResourceBarrier(1, &b);
    };

    // 1. SSAO 计算 → ambientRT0（法线/深度已由 System 绘制就绪）
    native->OMSetRenderTargets(1, &ambientRTV, TRUE, nullptr);
    ComputeAO(cmdList, aoPSO, depthSRV, normalSRV);

    // 过渡 ambientRT0: RENDER_TARGET → PIXEL_SHADER_RESOURCE（供 BlurAO 作为 SRV 读入）
    barrier(ambientRes0, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

    // 2. 水平模糊：读 ambientRT0 → 写 ambientRT1
    native->OMSetRenderTargets(1, &ambient1RTV, TRUE, nullptr);
    BlurAO(cmdList, blurPSO, true, ambientSRV);

    // 过渡 ambientRT1: RENDER_TARGET → PIXEL_SHADER_RESOURCE（供垂直 BlurAO 作为 SRV 读入）
    barrier(ambientRes1, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

    // 垂直模糊前：ambientRT0 从 PIXEL_SHADER_RESOURCE 转回 RENDER_TARGET（供写入）
    barrier(ambientRes0, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);

    // 3. 垂直模糊：读 ambientRT1 → 写 ambientRT0（最终结果）
    native->OMSetRenderTargets(1, &ambientRTV, TRUE, nullptr);
    BlurAO(cmdList, blurPSO, false, ambient1SRV);

    // 垂直模糊结束，ambientRT1 回归 RENDER_TARGET（供外层屏障统一管理）
    barrier(ambientRes1, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
}

void SsaoRenderer::Resize(uint32_t width, uint32_t height) {
    m_width = width;
    m_height = height;
}

// ========================================================================
// 内部：构建根签名
// ========================================================================

void SsaoRenderer::BuildRootSignatures() {
    auto *device = m_deviceContext->GetDevice();
    if (!device)
        return;

    // ── AO 计算根签名 ──
    //   RootCBV(b0): SSAO params (Proj, InvProj, ...)
    //   DescriptorTable(t0): NormalMap SRV
    //   DescriptorTable(t1): DepthMap SRV
    //   DescriptorTable(t2): RandomVector SRV
    {
        CD3DX12_ROOT_PARAMETER params[4];

        // [0]: CBV b0
        params[0].InitAsConstantBufferView(0);

        // [1-3]: 描述符表（每个表绑定一个 SRV）
        CD3DX12_DESCRIPTOR_RANGE ranges[3];
        ranges[0].Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0); // t0: NormalMap
        ranges[1].Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 1); // t1: DepthMap
        ranges[2].Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 2); // t2: RandomVec

        params[1].InitAsDescriptorTable(1, &ranges[0]);
        params[2].InitAsDescriptorTable(1, &ranges[1]);
        params[3].InitAsDescriptorTable(1, &ranges[2]);

        // 静态采样器
        CD3DX12_STATIC_SAMPLER_DESC samplers[4] = {
            CD3DX12_STATIC_SAMPLER_DESC(0, D3D12_FILTER_MIN_MAG_MIP_POINT),        // s0: point clamp
            CD3DX12_STATIC_SAMPLER_DESC(1, D3D12_FILTER_MIN_MAG_LINEAR_MIP_POINT), // s1: linear clamp
            CD3DX12_STATIC_SAMPLER_DESC(2, D3D12_FILTER_MIN_MAG_MIP_POINT,         // s2: depth map
                                        D3D12_TEXTURE_ADDRESS_MODE_CLAMP, D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
                                        D3D12_TEXTURE_ADDRESS_MODE_CLAMP),
            CD3DX12_STATIC_SAMPLER_DESC(3, D3D12_FILTER_MIN_MAG_LINEAR_MIP_POINT, // s3: linear wrap
                                        D3D12_TEXTURE_ADDRESS_MODE_WRAP, D3D12_TEXTURE_ADDRESS_MODE_WRAP,
                                        D3D12_TEXTURE_ADDRESS_MODE_WRAP),
        };

        CD3DX12_ROOT_SIGNATURE_DESC rootSigDesc;
        rootSigDesc.Init(4, params, 4, samplers, D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT);

        Microsoft::WRL::ComPtr<ID3DBlob> serialized, error;
        D3D12SerializeRootSignature(&rootSigDesc, D3D_ROOT_SIGNATURE_VERSION_1, &serialized, &error);
        device->CreateRootSignature(0, serialized->GetBufferPointer(), serialized->GetBufferSize(),
                                    IID_PPV_ARGS(&m_aoRootSig));
    }

    // ── 模糊根签名 ──
    //   RootConstants(b0, 1寄存器): gHorizontalBlur (bool)
    //   DescriptorTable(t0): Input AO map SRV
    {
        CD3DX12_ROOT_PARAMETER params[2];

        // [0]: 根常量 b0, 4 个 32-bit 值（HLSL cbuffer 最小单位 16 字节 = 1 float4）
        params[0].InitAsConstants(4, 0);

        // [1]: 描述符表 t0: 输入 AO 贴图
        CD3DX12_DESCRIPTOR_RANGE range;
        range.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0);
        params[1].InitAsDescriptorTable(1, &range);

        CD3DX12_STATIC_SAMPLER_DESC sampler(0, D3D12_FILTER_MIN_MAG_MIP_POINT); // s0: point clamp

        CD3DX12_ROOT_SIGNATURE_DESC rootSigDesc;
        rootSigDesc.Init(2, params, 1, &sampler, D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT);

        Microsoft::WRL::ComPtr<ID3DBlob> serialized, error;
        D3D12SerializeRootSignature(&rootSigDesc, D3D_ROOT_SIGNATURE_VERSION_1, &serialized, &error);
        device->CreateRootSignature(0, serialized->GetBufferPointer(), serialized->GetBufferSize(),
                                    IID_PPV_ARGS(&m_blurRootSig));
    }
}

// ========================================================================
// 内部：创建 SSAO + Blur PSO
// ========================================================================

void SsaoRenderer::CreatePipelines() {
    auto *device = m_deviceContext->GetDevice();
    if (!device || !m_aoRootSig || !m_blurRootSig)
        return;

    // 编译着色器
    UINT flags = D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION | D3DCOMPILE_ENABLE_UNBOUNDED_DESCRIPTOR_TABLES;
    Microsoft::WRL::ComPtr<ID3DBlob> vsSSAO, psSSAO, vsBlur, psBlur, errors;

    // SSAO
    HRESULT hr = D3DCompileFromFile(L"Shaders/Ssao.hlsl", nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE, "VS", "vs_5_1",
                                    flags, 0, &vsSSAO, &errors);
    if (FAILED(hr)) {
        if (errors)
            OutputDebugStringA((const char *)errors->GetBufferPointer());
        return;
    }
    hr = D3DCompileFromFile(L"Shaders/Ssao.hlsl", nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE, "PS", "ps_5_1", flags, 0,
                            &psSSAO, &errors);
    if (FAILED(hr)) {
        if (errors)
            OutputDebugStringA((const char *)errors->GetBufferPointer());
        return;
    }

    // Blur
    hr = D3DCompileFromFile(L"Shaders/SsaoBlur.hlsl", nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE, "VS", "vs_5_1", flags,
                            0, &vsBlur, &errors);
    if (FAILED(hr)) {
        if (errors)
            OutputDebugStringA((const char *)errors->GetBufferPointer());
        return;
    }
    hr = D3DCompileFromFile(L"Shaders/SsaoBlur.hlsl", nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE, "PS", "ps_5_1", flags,
                            0, &psBlur, &errors);
    if (FAILED(hr)) {
        if (errors)
            OutputDebugStringA((const char *)errors->GetBufferPointer());
        return;
    }

    // 创建 SSAO PSO
    D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc = {};
    psoDesc.InputLayout = {nullptr, 0};
    psoDesc.pRootSignature = m_aoRootSig.Get();
    psoDesc.VS = {vsSSAO->GetBufferPointer(), vsSSAO->GetBufferSize()};
    psoDesc.PS = {psSSAO->GetBufferPointer(), psSSAO->GetBufferSize()};
    psoDesc.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
    psoDesc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
    psoDesc.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
    psoDesc.DepthStencilState.DepthEnable = FALSE;
    psoDesc.SampleMask = UINT_MAX;
    psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    psoDesc.NumRenderTargets = 1;
    psoDesc.RTVFormats[0] = DXGI_FORMAT_R16_UNORM;
    psoDesc.SampleDesc.Count = 1;
    psoDesc.SampleDesc.Quality = 0;
    device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&m_ssaoPSO));

    // 创建 Blur PSO（使用 SsaoBlur 着色器 + blur 根签名）
    psoDesc.pRootSignature = m_blurRootSig.Get();
    psoDesc.VS = {vsBlur->GetBufferPointer(), vsBlur->GetBufferSize()};
    psoDesc.PS = {psBlur->GetBufferPointer(), psBlur->GetBufferSize()};
    device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&m_blurPSO));
}

// ========================================================================
// 内部：SSAO 计算
// ========================================================================

void SsaoRenderer::ComputeAO(CommandList &cmdList, ID3D12PipelineState *aoPSO, D3D12_GPU_DESCRIPTOR_HANDLE depthSRV,
                             D3D12_GPU_DESCRIPTOR_HANDLE normalSRV) {
    if (!aoPSO || !m_aoRootSig || !m_ssaoCB)
        return;

    auto *native = cmdList.Get();

    // 设置描述符堆
    ID3D12DescriptorHeap *heap = m_descriptorHeaps->GetHeap(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    native->SetDescriptorHeaps(1, &heap);

    native->SetGraphicsRootSignature(m_aoRootSig.Get());
    native->SetPipelineState(aoPSO);

    // CBV: SSAO 参数
    native->SetGraphicsRootConstantBufferView(0, m_ssaoCB->GetGPUVirtualAddress());

    // 描述符表：法线、深度、随机纹理
    native->SetGraphicsRootDescriptorTable(1, normalSRV);
    native->SetGraphicsRootDescriptorTable(2, depthSRV);
    native->SetGraphicsRootDescriptorTable(3, m_randomVectorMapSRV);

    // 全屏四边形（6 顶点，SV_VertexID 生成）
    native->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    native->DrawInstanced(6, 1, 0, 0);
}

// ========================================================================
// 内部：边缘保持模糊
// ========================================================================

void SsaoRenderer::BlurAO(CommandList &cmdList, ID3D12PipelineState *blurPSO, bool horizontal,
                          D3D12_GPU_DESCRIPTOR_HANDLE srcSRV) {
    if (!blurPSO || !m_blurRootSig)
        return;

    auto *native = cmdList.Get();

    ID3D12DescriptorHeap *heap = m_descriptorHeaps->GetHeap(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    native->SetDescriptorHeaps(1, &heap);

    native->SetGraphicsRootSignature(m_blurRootSig.Get());
    native->SetPipelineState(blurPSO);

    // 根常量：4 个 DWORD = [gHorizontalBlur, gTexelSize.x, gTexelSize.y, pad]
    float invW = 1.0f / static_cast<float>(m_width);
    float invH = 1.0f / static_cast<float>(m_height);
    uint32_t rc[4] = {};
    rc[0] = horizontal ? 1u : 0u;
    memcpy(&rc[1], &invW, sizeof(float));
    memcpy(&rc[2], &invH, sizeof(float));
    native->SetGraphicsRoot32BitConstants(0, 4, rc, 0);

    // 描述符表：输入 AO 贴图
    native->SetGraphicsRootDescriptorTable(1, srcSRV);

    native->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    native->DrawInstanced(6, 1, 0, 0);
}

} // namespace DX12Engine::Renderer
