#include "LightingRenderer.h"
#include "Renderer/RHI/D3D12DeviceContext.h"
#include <d3dcompiler.h>

using namespace DX12Engine::Renderer;

// ========================================================================
// 生命周期
// ========================================================================

void LightingRenderer::SetDeviceContext(D3D12DeviceContext *context) { m_context = context; }

void LightingRenderer::Initialize() {
    if (!m_context) {
        throw std::runtime_error("LightingRenderer: Device context not set before Initialize");
    }

    LoadShaders();

    if (!m_vsBlob || !m_psBlob) {
        OutputDebugStringW(L"[ERROR] LightingRenderer: Failed to load shaders!\n");
        throw std::runtime_error("LightingRenderer: Failed to load shaders");
    }

    CreateRootSignature();
    CreatePSO();

    OutputDebugStringW(L"[INFO] LightingRenderer initialized successfully\n");
}

void LightingRenderer::OnResize(uint32_t width, uint32_t height) {}

void LightingRenderer::Update(float deltaTime) {}

// ========================================================================
// 着色器加载（带完整错误报告）
// ========================================================================

void LightingRenderer::LoadShaders() {
    UINT compileFlags = D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION | D3DCOMPILE_ENABLE_UNBOUNDED_DESCRIPTOR_TABLES;

    Microsoft::WRL::ComPtr<ID3DBlob> errors;
    HRESULT hr;

    hr = D3DCompileFromFile(L"Shaders/lighting.hlsl", nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE,
                            "VS", "vs_5_1", compileFlags, 0, &m_vsBlob, &errors);
    if (FAILED(hr)) {
        if (errors) {
            const char *errMsg = reinterpret_cast<const char *>(errors->GetBufferPointer());
            OutputDebugStringA("=== LIGHTING VS COMPILATION ERROR ===\n");
            OutputDebugStringA(errMsg);
            OutputDebugStringA("======================================\n");
        }
        throw std::runtime_error("LightingRenderer: Failed to compile Vertex Shader");
    }

    errors = nullptr;
    hr = D3DCompileFromFile(L"Shaders/lighting.hlsl", nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE,
                            "PS", "ps_5_1", compileFlags, 0, &m_psBlob, &errors);
    if (FAILED(hr)) {
        if (errors) {
            const char *errMsg = reinterpret_cast<const char *>(errors->GetBufferPointer());
            OutputDebugStringA("=== LIGHTING PS COMPILATION ERROR ===\n");
            OutputDebugStringA(errMsg);
            OutputDebugStringA("======================================\n");
        }
        throw std::runtime_error("LightingRenderer: Failed to compile Pixel Shader");
    }

    OutputDebugStringW(L"[INFO] LightingRenderer shaders compiled successfully\n");
}

// ========================================================================
// 根签名
// ========================================================================

void LightingRenderer::CreateRootSignature() {
    auto device = m_context->GetDevice();

    // 根参数布局:
    //   slot 0: b1  cbPass                                  (CBV)
    //   slot 1: b2  cbLights                                (CBV)
    //   slot 2: t20 Albedo RT                               (SRV 描述符表)
    //   slot 3: t21 Normal RT                               (SRV 描述符表)
    //   slot 4: t22 Material RT                             (SRV 描述符表)
    //   slot 5: t23 WorldPos RT                             (SRV 描述符表)
    //   slot 6: t16 SSAO Map                                (SRV 描述符表)
    //   slot 7: t10 EnvMap                                  (SRV 描述符表)
    //   slot 8: t15 ReflectionCubemapArray                  (SRV 描述符表)
    //   slot 9: t11,space1 ShadowParams                        (SRV 描述符表)
    //   slot 10: t14,space1 gShadowMaps[]                      (无界纹理数组 SRV)
    CD3DX12_ROOT_PARAMETER params[11];

    params[0].InitAsConstantBufferView(1, 0, D3D12_SHADER_VISIBILITY_ALL);
    params[1].InitAsConstantBufferView(2, 0, D3D12_SHADER_VISIBILITY_PIXEL);

    CD3DX12_DESCRIPTOR_RANGE rtRanges[9];
    for (uint32_t i = 0; i < 5; ++i)
        rtRanges[i].Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, (i < 4) ? 20 + i : 16, 0);
    rtRanges[5].Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 10, 0); // t10: EnvMap
    rtRanges[6].Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 15, 0); // t15: Cubemap Array
    rtRanges[7].Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 11, 1); // t11,space1: ShadowParams
    rtRanges[8].Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, UINT_MAX, 14, 1); // t14,space1: gShadowMaps[] (无界)
    for (uint32_t i = 0; i < 9; ++i)
        params[2 + i].InitAsDescriptorTable(1, &rtRanges[i], D3D12_SHADER_VISIBILITY_PIXEL);

    // 静态采样器 s3: PointClamp, s10: EnvMap 线性 Clamp, s11: 阴影比较
    CD3DX12_STATIC_SAMPLER_DESC staticSamplers[3];
    staticSamplers[0].Init(3, D3D12_FILTER_MIN_MAG_MIP_POINT,
                           D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
                           D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
                           D3D12_TEXTURE_ADDRESS_MODE_CLAMP);
    staticSamplers[1].Init(10, D3D12_FILTER_MIN_MAG_MIP_LINEAR,
                           D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
                           D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
                           D3D12_TEXTURE_ADDRESS_MODE_CLAMP);
    staticSamplers[2].Init(11, D3D12_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT,
                           D3D12_TEXTURE_ADDRESS_MODE_BORDER,
                           D3D12_TEXTURE_ADDRESS_MODE_BORDER,
                           D3D12_TEXTURE_ADDRESS_MODE_BORDER, 0.0f, 0,
                           D3D12_COMPARISON_FUNC_LESS_EQUAL,
                           D3D12_STATIC_BORDER_COLOR_OPAQUE_BLACK);

    CD3DX12_ROOT_SIGNATURE_DESC rsDesc;
    rsDesc.Init(11, params, 3, staticSamplers,
                D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT);

    Microsoft::WRL::ComPtr<ID3DBlob> signature, error;
    HRESULT hr = D3D12SerializeRootSignature(&rsDesc, D3D_ROOT_SIGNATURE_VERSION_1, &signature, &error);
    if (error) {
        OutputDebugStringA(reinterpret_cast<const char *>(error->GetBufferPointer()));
    }
    ThrowIfFailed(hr);

    ThrowIfFailed(device->CreateRootSignature(0, signature->GetBufferPointer(),
                                              signature->GetBufferSize(), IID_PPV_ARGS(&m_rootSignature)));
}

// ========================================================================
// PSO — 无深度/模板、不剔除背面、单 RT 输出到交换链
// ========================================================================

void LightingRenderer::CreatePSO() {
    auto device = m_context->GetDevice();

    D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc = {};
    psoDesc.pRootSignature = m_rootSignature.Get();
    psoDesc.VS = {reinterpret_cast<BYTE *>(m_vsBlob->GetBufferPointer()), m_vsBlob->GetBufferSize()};
    psoDesc.PS = {reinterpret_cast<BYTE *>(m_psBlob->GetBufferPointer()), m_psBlob->GetBufferSize()};

    psoDesc.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
    psoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    psoDesc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);

    D3D12_DEPTH_STENCIL_DESC dsDesc = {};
    dsDesc.DepthEnable = FALSE;
    dsDesc.StencilEnable = FALSE;
    psoDesc.DepthStencilState = dsDesc;

    psoDesc.NumRenderTargets = 1;
    psoDesc.RTVFormats[0] = m_context->GetBackBufferFormat();
    psoDesc.SampleMask = UINT_MAX;
    psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    psoDesc.SampleDesc.Count = 1;

    ThrowIfFailed(device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&m_pso)));

    OutputDebugStringW(L"[INFO] LightingRenderer PSO created successfully\n");
}

// ========================================================================
// 帧渲染接口
// ========================================================================

void LightingRenderer::BeginFrame(CommandList &cmdList,
                                  D3D12_GPU_VIRTUAL_ADDRESS passConstantsAddress,
                                  D3D12_GPU_VIRTUAL_ADDRESS lightCBAddress,
                                  D3D12_GPU_DESCRIPTOR_HANDLE albedoSrv,
                                  D3D12_GPU_DESCRIPTOR_HANDLE normalSrv,
                                  D3D12_GPU_DESCRIPTOR_HANDLE materialSrv,
                                  D3D12_GPU_DESCRIPTOR_HANDLE worldPosSrv,
                                  D3D12_GPU_DESCRIPTOR_HANDLE ssaoSrv,
                                  D3D12_GPU_DESCRIPTOR_HANDLE envMapSrv,
                                  D3D12_GPU_DESCRIPTOR_HANDLE cubemapArraySrv,
                                  D3D12_GPU_DESCRIPTOR_HANDLE shadowDataSRV,
                                  D3D12_GPU_DESCRIPTOR_HANDLE shadowMapSRV) {
    if (!m_pso || !m_rootSignature) return;

    cmdList.Get()->SetPipelineState(m_pso.Get());
    cmdList.Get()->SetGraphicsRootSignature(m_rootSignature.Get());
    cmdList.Get()->SetGraphicsRootConstantBufferView(0, passConstantsAddress);
    cmdList.Get()->SetGraphicsRootConstantBufferView(1, lightCBAddress);

    // slot 2-5: G-buffer RT SRV
    if (albedoSrv.ptr != 0)
        cmdList.Get()->SetGraphicsRootDescriptorTable(2, albedoSrv);
    if (normalSrv.ptr != 0)
        cmdList.Get()->SetGraphicsRootDescriptorTable(3, normalSrv);
    if (materialSrv.ptr != 0)
        cmdList.Get()->SetGraphicsRootDescriptorTable(4, materialSrv);
    if (worldPosSrv.ptr != 0)
        cmdList.Get()->SetGraphicsRootDescriptorTable(5, worldPosSrv);

    // slot 6: SSAO
    if (ssaoSrv.ptr != 0)
        cmdList.Get()->SetGraphicsRootDescriptorTable(6, ssaoSrv);

    // slot 7: 环境贴图
    if (envMapSrv.ptr != 0)
        cmdList.Get()->SetGraphicsRootDescriptorTable(7, envMapSrv);

    // slot 8: 反射探针 Cubemap Array
    if (cubemapArraySrv.ptr != 0)
        cmdList.Get()->SetGraphicsRootDescriptorTable(8, cubemapArraySrv);

    // slot 9: 阴影采样参数 (ShadowParams)
    if (shadowDataSRV.ptr != 0)
        cmdList.Get()->SetGraphicsRootDescriptorTable(9, shadowDataSRV);

    // slot 10: 阴影贴图无界数组 gShadowMaps[]（方向光/点光源共用）
    if (shadowMapSRV.ptr != 0)
        cmdList.Get()->SetGraphicsRootDescriptorTable(10, shadowMapSRV);
}

void LightingRenderer::Draw(CommandList &cmdList) {
    if (!m_pso) return;
    cmdList.Get()->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    cmdList.Get()->DrawInstanced(4, 1, 0, 0);
}

void LightingRenderer::EndFrame() {}
