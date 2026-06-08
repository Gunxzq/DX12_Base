#include "ShadowRenderer.h"
#include "Common/d3dUtil.h"
#include "Renderer/RHI/D3D12DeviceContext.h"
#include "Resource/Geometry/TriangleMesh.h"
#include "Resource/GpuResourceManager.h"
#include "Resource/Manager/GeometryResourceManager.h"
#include <d3dcompiler.h>

using namespace DirectX;
using namespace DX12Engine::Resource;

namespace DX12Engine::Renderer {

// ============================================================================
// OffscreenRenderer 生命周期管理
// ============================================================================

void ShadowRenderer::SetDeviceContext(D3D12DeviceContext *context) { m_context = context; }

void ShadowRenderer::Initialize() {
    if (!m_context) {
        throw std::runtime_error("ShadowRenderer: Device context not set before Initialize");
    }

    LoadShaders();
    CreateRootSignature();
    CreatePSO();

    OutputDebugStringW(L"[INFO] ShadowRenderer initialized successfully\n");
}

void ShadowRenderer::Shutdown() {
    m_pso.Reset();
    m_rootSignature.Reset();
    m_vsBlob.Reset();
    m_psBlob.Reset();
    m_inPass = false;
    m_context = nullptr;
    m_geometryManager = nullptr;
}

void ShadowRenderer::Resize(uint32_t width, uint32_t height) {
    m_passWidth = width;
    m_passHeight = height;
    OutputDebugStringW((L"[INFO] ShadowRenderer::Resize to " + std::to_wstring(width) + L"x" +
                        std::to_wstring(height) + L"\n").c_str());
}

// ============================================================================
// OffscreenRenderer 接口 — 离屏渲染核心
// ============================================================================

void ShadowRenderer::SetShadowPassParams(D3D12_GPU_VIRTUAL_ADDRESS lightCBAddress,
                                         D3D12_CPU_DESCRIPTOR_HANDLE dsvHandle, uint32_t width, uint32_t height) {
    m_cachedLightCBAddress = lightCBAddress;
    m_passWidth = width;
    m_passHeight = height;
    m_currentDsvHandle = dsvHandle;
}

void ShadowRenderer::BeginOffscreen(CommandList &cmdList) {
    BeginOffscreen(cmdList, m_cachedLightCBAddress, m_currentDsvHandle, m_passWidth, m_passHeight);
}

void ShadowRenderer::BeginOffscreen(CommandList &cmdList, D3D12_GPU_VIRTUAL_ADDRESS lightCBAddress,
                                    D3D12_CPU_DESCRIPTOR_HANDLE dsvHandle, uint32_t width, uint32_t height) {
    if (!m_pso || !m_rootSignature) {
        OutputDebugStringW(L"[ERROR] ShadowRenderer::BeginOffscreen: PSO or RootSignature not initialized\n");
        return;
    }

    m_passWidth = width;
    m_passHeight = height;
    m_currentDsvHandle = dsvHandle;

    D3D12_VIEWPORT viewport = {0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height), 0.0f, 1.0f};
    D3D12_RECT scissorRect = {0, 0, static_cast<LONG>(width), static_cast<LONG>(height)};
    cmdList.Get()->RSSetViewports(1, &viewport);
    cmdList.Get()->RSSetScissorRects(1, &scissorRect);

    cmdList.Get()->ClearDepthStencilView(dsvHandle, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
    cmdList.Get()->OMSetRenderTargets(0, nullptr, FALSE, &dsvHandle);

    cmdList.Get()->SetPipelineState(m_pso.Get());
    cmdList.Get()->SetGraphicsRootSignature(m_rootSignature.Get());
    cmdList.Get()->SetGraphicsRootConstantBufferView(1, lightCBAddress);

    m_inPass = true;
}

void ShadowRenderer::EndOffscreen(CommandList &cmdList) {
    if (!m_inPass) {
        OutputDebugStringW(L"[WARN] ShadowRenderer::EndOffscreen: Called without matching BeginOffscreen\n");
        return;
    }
    m_inPass = false;
    (void)cmdList;
}

// ========================================================================
// OffscreenRenderer 接口 — 输出纹理访问
// ========================================================================

D3D12_GPU_DESCRIPTOR_HANDLE ShadowRenderer::GetOutputSRV() const {
    return {};
}

D3D12_CPU_DESCRIPTOR_HANDLE ShadowRenderer::GetOutputRTV() const {
    return {};
}

D3D12_CPU_DESCRIPTOR_HANDLE ShadowRenderer::GetDepthDSV() const {
    return m_currentDsvHandle;
}

void ShadowRenderer::DrawInstanced(CommandList &cmdList, GeometryHandle geometryHandle,
                                   D3D12_GPU_VIRTUAL_ADDRESS instanceBufferAddress, uint32_t instanceCount) {
    if (!m_inPass || !m_geometryManager) {
        OutputDebugStringW(L"[ERROR] ShadowRenderer::DrawInstanced: BeginOffscreen not called\n");
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
        OutputDebugStringW(L"[ERROR] ShadowRenderer::DrawInstanced - Invalid buffer!\n");
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

    // 绑定 InstanceData StructuredBuffer (slot 2, t12,space1)
    cmdList.Get()->SetGraphicsRootShaderResourceView(2, instanceBufferAddress);

    // 执行实例化绘制
    cmdList.Get()->DrawIndexedInstanced(mesh->indexCount, instanceCount, 0, 0, 0);
}

// ============================================================================
// 内部初始化
// ============================================================================

void ShadowRenderer::LoadShaders() {
    UINT compileFlags = D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
    compileFlags |= D3DCOMPILE_ENABLE_UNBOUNDED_DESCRIPTOR_TABLES;
    Microsoft::WRL::ComPtr<ID3DBlob> errors = nullptr;
    HRESULT hr;

    // 方向光 VS — 统一实例化模式
    hr = D3DCompileFromFile(L"Shaders/Shadow.hlsl", nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE, "DirShadowVS", "vs_5_1",
                            compileFlags, 0, &m_vsBlob, &errors);
    if (FAILED(hr)) {
        if (errors) {
            OutputDebugStringA(reinterpret_cast<const char *>(errors->GetBufferPointer()));
        }
        throw std::runtime_error("ShadowRenderer: Failed to compile DirShadowVS");
    }

    // 阴影 PS (入口: ShadowPS)
    hr = D3DCompileFromFile(L"Shaders/Shadow.hlsl", nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE, "ShadowPS", "ps_5_1",
                            compileFlags, 0, &m_psBlob, &errors);
    if (FAILED(hr)) {
        if (errors) {
            OutputDebugStringA(reinterpret_cast<const char *>(errors->GetBufferPointer()));
        }
        throw std::runtime_error("ShadowRenderer: Failed to compile ShadowPS");
    }

    OutputDebugStringW(L"[INFO] Shadow shaders compiled successfully\n");
}

void ShadowRenderer::CreateRootSignature() {
    auto device = m_context->GetDevice();

    // ========================================================================
    // 根参数布局 (对齐 Shadow.hlsl):
    //   slot 0: b0 cbShadowObject  (CBV — 点光源/聚光灯阴影 VS 使用)
    //   slot 1: b1 cbDirShadow     (CBV — 光源 VP 矩阵等)
    //   slot 2: t12,space1         (SRV — InstanceData StructuredBuffer)
    // ========================================================================
    CD3DX12_ROOT_PARAMETER slotRootParameter[3];

    slotRootParameter[0].InitAsConstantBufferView(0, 0, D3D12_SHADER_VISIBILITY_VERTEX); // b0
    slotRootParameter[1].InitAsConstantBufferView(1, 0, D3D12_SHADER_VISIBILITY_VERTEX); // b1
    slotRootParameter[2].InitAsShaderResourceView(
        12, 1, D3D12_SHADER_VISIBILITY_VERTEX); // t12,space1: InstanceData StructuredBuffer

    CD3DX12_ROOT_SIGNATURE_DESC rootSigDesc(3, slotRootParameter, 0, nullptr,
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

void ShadowRenderer::CreatePSO() {
    auto device = m_context->GetDevice();

    D3D12_INPUT_ELEMENT_DESC inputLayout[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0}};

    D3D12_DEPTH_STENCIL_DESC depthStencilDesc = {};
    depthStencilDesc.DepthEnable = TRUE;
    depthStencilDesc.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    depthStencilDesc.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
    depthStencilDesc.StencilEnable = FALSE;

    D3D12_RASTERIZER_DESC rasterizerDesc = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
    rasterizerDesc.DepthBias = 100;
    rasterizerDesc.DepthBiasClamp = 0.0f;
    rasterizerDesc.SlopeScaledDepthBias = 10.0f;
    rasterizerDesc.CullMode = D3D12_CULL_MODE_BACK;

    D3D12_BLEND_DESC blendDesc = CD3DX12_BLEND_DESC(D3D12_DEFAULT);

    D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc = {};
    psoDesc.InputLayout = {inputLayout, _countof(inputLayout)};
    psoDesc.pRootSignature = m_rootSignature.Get();
    psoDesc.VS = {reinterpret_cast<BYTE *>(m_vsBlob->GetBufferPointer()), m_vsBlob->GetBufferSize()};
    psoDesc.PS = {reinterpret_cast<BYTE *>(m_psBlob->GetBufferPointer()), m_psBlob->GetBufferSize()};
    psoDesc.RasterizerState = rasterizerDesc;
    psoDesc.BlendState = blendDesc;
    psoDesc.DepthStencilState = depthStencilDesc;
    psoDesc.SampleMask = UINT_MAX;
    psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    psoDesc.NumRenderTargets = 0;
    psoDesc.RTVFormats[0] = DXGI_FORMAT_UNKNOWN;
    psoDesc.DSVFormat = DXGI_FORMAT_D32_FLOAT;
    psoDesc.SampleDesc.Count = 1;
    psoDesc.SampleDesc.Quality = 0;

    ThrowIfFailed(device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&m_pso)));

    OutputDebugStringW(L"[INFO] Shadow PSO created successfully\n");
}

} // namespace DX12Engine::Renderer
