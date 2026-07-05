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

    LoadPointInstancedShaders();
    CreatePointInstancedPSO();

    LoadPointGSShaders();
    CreatePointGSPSO();

    LoadSpotShaders();
    CreateSpotPSO();

    OutputDebugStringW(L"[INFO] ShadowRenderer initialized (directional + point + spot)\n");
}

void ShadowRenderer::Shutdown() {
    m_pso.Reset();
    m_rootSignature.Reset();
    m_vsBlob.Reset();
    m_psBlob.Reset();

    m_pointInstancedPSO.Reset();
    m_pointInstancedVSBlob.Reset();

    m_pointGSPSO.Reset();
    m_pointGSVSBlob.Reset();
    m_pointGSGSBlob.Reset();
    m_pointGSPSBlob.Reset();

    m_spotPSO.Reset();
    m_spotVSBlob.Reset();

    m_inPass = false;
    m_context = nullptr;
    m_geometryManager = nullptr;
}

void ShadowRenderer::Resize(uint32_t width, uint32_t height) {
    m_passWidth = width;
    m_passHeight = height;
    OutputDebugStringW(
        (L"[INFO] ShadowRenderer::Resize to " + std::to_wstring(width) + L"x" + std::to_wstring(height) + L"\n")
            .c_str());
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
        ErrorReporter::Report("ShadowRenderer::BeginOffscreen: PSO or RootSignature not initialized");
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

D3D12_GPU_DESCRIPTOR_HANDLE ShadowRenderer::GetOutputSRV() const { return {}; }

D3D12_CPU_DESCRIPTOR_HANDLE ShadowRenderer::GetOutputRTV() const { return {}; }

D3D12_CPU_DESCRIPTOR_HANDLE ShadowRenderer::GetDepthDSV() const { return m_currentDsvHandle; }

void ShadowRenderer::DrawInstanced(CommandList &cmdList, GeometryHandle geometryHandle,
                                   D3D12_GPU_VIRTUAL_ADDRESS instanceBufferAddress, uint32_t instanceCount) {
    if (!m_inPass || !m_geometryManager) {
        ErrorReporter::Report("ShadowRenderer::DrawInstanced: BeginOffscreen not called");
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
        ErrorReporter::Report("ShadowRenderer::DrawInstanced - Invalid buffer");
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

void ShadowRenderer::DrawIndexedInstancedSubmesh(CommandList &cmdList, GeometryHandle geometryHandle,
                                                 D3D12_GPU_VIRTUAL_ADDRESS instanceBufferAddress,
                                                 uint32_t instanceCount, uint32_t startIndex, int32_t startVertex,
                                                 uint32_t indexCount) {
    if (!m_inPass || !m_geometryManager) {
        ErrorReporter::Report("ShadowRenderer::DrawIndexedInstancedSubmesh: BeginOffscreen not called");
        return;
    }

    const TriangleMesh *mesh = m_geometryManager->GetGeometry<TriangleMesh>(geometryHandle);
    if (!mesh || !mesh->isGpuReady)
        return;

    auto &gpuMgr = GpuResourceManager::GetInstance();
    ID3D12Resource *vbResource = gpuMgr.GetResource(mesh->vertexBufferHandle);
    ID3D12Resource *ibResource = gpuMgr.GetResource(mesh->indexBufferHandle);
    if (!vbResource || !ibResource)
        return;

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

    cmdList.Get()->SetGraphicsRootShaderResourceView(2, instanceBufferAddress);

    uint32_t actualIndexCount = indexCount > 0 ? indexCount : mesh->indexCount;
    cmdList.Get()->DrawIndexedInstanced(actualIndexCount, instanceCount, startIndex, startVertex, 0);
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
    //   slot 0: b0 cbShadowObject   (CBV — VS) 点光源单面/GS 路径使用 gWorld
    //   slot 1: b1 cbDirShadow / cbPointShadow (CBV — VS+GS) 光源 VP 矩阵
    //   slot 2: t12,space1          (SRV — VS) StructuredBuffer<InstanceData>
    //   slot 3: b2                  (root constant — VS) gShadowLightIndex
    // ========================================================================
    CD3DX12_ROOT_PARAMETER slotRootParameter[4];

    slotRootParameter[0].InitAsConstantBufferView(0, 0, D3D12_SHADER_VISIBILITY_VERTEX); // b0
    slotRootParameter[1].InitAsConstantBufferView(1, 0, D3D12_SHADER_VISIBILITY_ALL);    // b1: GS 也需读取
    slotRootParameter[2].InitAsShaderResourceView(
        12, 1, D3D12_SHADER_VISIBILITY_VERTEX); // t12,space1: InstanceData StructuredBuffer
    slotRootParameter[3].InitAsConstants(1, 2, 0, D3D12_SHADER_VISIBILITY_VERTEX); // b2: gShadowLightIndex

    CD3DX12_ROOT_SIGNATURE_DESC rootSigDesc(4, slotRootParameter, 0, nullptr,
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
    rasterizerDesc.CullMode = D3D12_CULL_MODE_NONE; // 阴影贴图：不剔除背面，避免视角依赖的深度缺失

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

// ========================================================================
// 点光源阴影（实例化，单面）着色器加载
// ========================================================================

void ShadowRenderer::LoadPointInstancedShaders() {
    UINT compileFlags = D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
    compileFlags |= D3DCOMPILE_ENABLE_UNBOUNDED_DESCRIPTOR_TABLES;
    Microsoft::WRL::ComPtr<ID3DBlob> errors;

    HRESULT hr =
        D3DCompileFromFile(L"Shaders/Shadow.hlsl", nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE,
                           "PointShadowVS_Instanced", "vs_5_1", compileFlags, 0, &m_pointInstancedVSBlob, &errors);
    if (FAILED(hr)) {
        if (errors)
            OutputDebugStringA(reinterpret_cast<const char *>(errors->GetBufferPointer()));
        throw std::runtime_error("ShadowRenderer: Failed to compile PointShadowVS_Instanced");
    }
    // 复用 ShadowPS + 方向光根签名
}

// ========================================================================
// 点光源阴影 PSO（实例化，单面，复用方向光根签名）
// ========================================================================

void ShadowRenderer::CreatePointInstancedPSO() {
    auto device = m_context->GetDevice();

    D3D12_INPUT_ELEMENT_DESC inputLayout[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0}};
    D3D12_DEPTH_STENCIL_DESC dsDesc = {};
    dsDesc.DepthEnable = TRUE;
    dsDesc.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    dsDesc.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
    dsDesc.StencilEnable = FALSE;
    D3D12_RASTERIZER_DESC rasterDesc = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
    rasterDesc.DepthBias = 100;
    rasterDesc.DepthBiasClamp = 0.0f;
    rasterDesc.SlopeScaledDepthBias = 5.0f;
    rasterDesc.CullMode = D3D12_CULL_MODE_NONE;

    D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc = {};
    psoDesc.InputLayout = {inputLayout, _countof(inputLayout)};
    psoDesc.pRootSignature = m_rootSignature.Get(); // 复用方向光根签名
    psoDesc.VS = {m_pointInstancedVSBlob->GetBufferPointer(), m_pointInstancedVSBlob->GetBufferSize()};
    psoDesc.PS = {m_psBlob->GetBufferPointer(), m_psBlob->GetBufferSize()}; // 复用 ShadowPS
    psoDesc.RasterizerState = rasterDesc;
    psoDesc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
    psoDesc.DepthStencilState = dsDesc;
    psoDesc.SampleMask = UINT_MAX;
    psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    psoDesc.NumRenderTargets = 0;
    psoDesc.DSVFormat = DXGI_FORMAT_D32_FLOAT;
    psoDesc.SampleDesc.Count = 1;
    psoDesc.SampleDesc.Quality = 0;
    ThrowIfFailed(device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&m_pointInstancedPSO)));
    OutputDebugStringW(L"[INFO] ShadowRenderer: Point instanced PSO created\n");
}

// ========================================================================
// 点光源阴影（GS 展开）着色器加载
// ========================================================================

void ShadowRenderer::LoadPointGSShaders() {
    UINT compileFlags = D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
    compileFlags |= D3DCOMPILE_ENABLE_UNBOUNDED_DESCRIPTOR_TABLES;
    Microsoft::WRL::ComPtr<ID3DBlob> errors;

    HRESULT hr;

    hr = D3DCompileFromFile(L"Shaders/Shadow.hlsl", nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE, "PointShadowVS_GS",
                            "vs_5_1", compileFlags, 0, &m_pointGSVSBlob, &errors);
    if (FAILED(hr)) {
        if (errors)
            OutputDebugStringA(reinterpret_cast<const char *>(errors->GetBufferPointer()));
        throw std::runtime_error("ShadowRenderer: Failed to compile PointShadowVS_GS");
    }

    hr = D3DCompileFromFile(L"Shaders/Shadow.hlsl", nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE, "PointShadowGS",
                            "gs_5_1", compileFlags, 0, &m_pointGSGSBlob, &errors);
    if (FAILED(hr)) {
        if (errors)
            OutputDebugStringA(reinterpret_cast<const char *>(errors->GetBufferPointer()));
        throw std::runtime_error("ShadowRenderer: Failed to compile PointShadowGS");
    }

    hr = D3DCompileFromFile(L"Shaders/Shadow.hlsl", nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE, "ShadowPS_Point",
                            "ps_5_1", compileFlags, 0, &m_pointGSPSBlob, &errors);
    if (FAILED(hr)) {
        if (errors)
            OutputDebugStringA(reinterpret_cast<const char *>(errors->GetBufferPointer()));
        throw std::runtime_error("ShadowRenderer: Failed to compile ShadowPS_Point");
    }
}

// ========================================================================
// 点光源阴影 PSO（GS 展开，复用方向光根签名）
// ========================================================================

void ShadowRenderer::CreatePointGSPSO() {
    auto device = m_context->GetDevice();

    D3D12_INPUT_ELEMENT_DESC inputLayout[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0}};
    D3D12_DEPTH_STENCIL_DESC dsDesc = {};
    dsDesc.DepthEnable = TRUE;
    dsDesc.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    dsDesc.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
    dsDesc.StencilEnable = FALSE;
    D3D12_RASTERIZER_DESC rasterDesc = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
    rasterDesc.DepthBias = 100;
    rasterDesc.DepthBiasClamp = 0.0f;
    rasterDesc.SlopeScaledDepthBias = 5.0f;
    rasterDesc.CullMode = D3D12_CULL_MODE_NONE;

    D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc = {};
    psoDesc.InputLayout = {inputLayout, _countof(inputLayout)};
    psoDesc.pRootSignature = m_rootSignature.Get();
    psoDesc.VS = {m_pointGSVSBlob->GetBufferPointer(), m_pointGSVSBlob->GetBufferSize()};
    psoDesc.GS = {m_pointGSGSBlob->GetBufferPointer(), m_pointGSGSBlob->GetBufferSize()};
    psoDesc.PS = {m_pointGSPSBlob->GetBufferPointer(), m_pointGSPSBlob->GetBufferSize()};
    psoDesc.RasterizerState = rasterDesc;
    psoDesc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
    psoDesc.DepthStencilState = dsDesc;
    psoDesc.SampleMask = UINT_MAX;
    psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    psoDesc.NumRenderTargets = 0;
    psoDesc.DSVFormat = DXGI_FORMAT_D32_FLOAT;
    psoDesc.SampleDesc.Count = 1;
    psoDesc.SampleDesc.Quality = 0;
    ThrowIfFailed(device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&m_pointGSPSO)));
    OutputDebugStringW(L"[INFO] ShadowRenderer: Point GS PSO created\n");
}

// ========================================================================
// 聚光灯阴影着色器加载（实例化，单面，复用方向光根签名）
// ========================================================================

void ShadowRenderer::LoadSpotShaders() {
    UINT compileFlags = D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
    compileFlags |= D3DCOMPILE_ENABLE_UNBOUNDED_DESCRIPTOR_TABLES;
    Microsoft::WRL::ComPtr<ID3DBlob> errors;

    HRESULT hr = D3DCompileFromFile(L"Shaders/Shadow.hlsl", nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE,
                                    "SpotShadowVS_Instanced", "vs_5_1", compileFlags, 0, &m_spotVSBlob, &errors);
    if (FAILED(hr)) {
        if (errors)
            OutputDebugStringA(reinterpret_cast<const char *>(errors->GetBufferPointer()));
        throw std::runtime_error("ShadowRenderer: Failed to compile SpotShadowVS_Instanced");
    }
    // 复用 ShadowPS + 方向光根签名
}

void ShadowRenderer::CreateSpotPSO() {
    auto device = m_context->GetDevice();

    D3D12_INPUT_ELEMENT_DESC inputLayout[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0}};
    D3D12_DEPTH_STENCIL_DESC dsDesc = {};
    dsDesc.DepthEnable = TRUE;
    dsDesc.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    dsDesc.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
    dsDesc.StencilEnable = FALSE;
    D3D12_RASTERIZER_DESC rasterDesc = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
    rasterDesc.DepthBias = 100;
    rasterDesc.DepthBiasClamp = 0.0f;
    rasterDesc.SlopeScaledDepthBias = 10.0f;
    rasterDesc.CullMode = D3D12_CULL_MODE_NONE;

    D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc = {};
    psoDesc.InputLayout = {inputLayout, _countof(inputLayout)};
    psoDesc.pRootSignature = m_rootSignature.Get();
    psoDesc.VS = {m_spotVSBlob->GetBufferPointer(), m_spotVSBlob->GetBufferSize()};
    psoDesc.PS = {m_psBlob->GetBufferPointer(), m_psBlob->GetBufferSize()};
    psoDesc.RasterizerState = rasterDesc;
    psoDesc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
    psoDesc.DepthStencilState = dsDesc;
    psoDesc.SampleMask = UINT_MAX;
    psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    psoDesc.NumRenderTargets = 0;
    psoDesc.DSVFormat = DXGI_FORMAT_D32_FLOAT;
    psoDesc.SampleDesc.Count = 1;
    psoDesc.SampleDesc.Quality = 0;
    ThrowIfFailed(device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&m_spotPSO)));
    OutputDebugStringW(L"[INFO] ShadowRenderer: Spot PSO created\n");
}

} // namespace DX12Engine::Renderer
