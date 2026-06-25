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
                                D3D12_GPU_VIRTUAL_ADDRESS lightCBAddress, D3D12_GPU_DESCRIPTOR_HANDLE materialBufferSRV,
                                D3D12_GPU_DESCRIPTOR_HANDLE shadowDataSRV, D3D12_GPU_DESCRIPTOR_HANDLE shadowMapSRV,
                                D3D12_GPU_DESCRIPTOR_HANDLE cubemapArraySRV) {
    if (!m_pso || !m_rootSignature)
        return;

    cmdList.Get()->SetPipelineState(m_pso.Get());
    cmdList.Get()->SetGraphicsRootSignature(m_rootSignature.Get());
    cmdList.Get()->SetGraphicsRootConstantBufferView(0, passConstantsAddress);
    cmdList.Get()->SetGraphicsRootConstantBufferView(1, lightCBAddress);

    // 绑定材质数组 SRV (slot 2)
    if (materialBufferSRV.ptr != 0) {
        cmdList.Get()->SetGraphicsRootDescriptorTable(2, materialBufferSRV);
    }

    // 绑定阴影数据 StructuredBuffer SRV (slot 4, t11,space1)
    if (shadowDataSRV.ptr != 0) {
        cmdList.Get()->SetGraphicsRootDescriptorTable(4, shadowDataSRV);
    }

    // 绑定阴影贴图 SRV (slot 5, t14,space1)
    if (shadowMapSRV.ptr != 0) {
        cmdList.Get()->SetGraphicsRootDescriptorTable(5, shadowMapSRV);
    }

    // 绑定反射探针 Cubemap Array SRV (slot 7, t10)
    if (cubemapArraySRV.ptr != 0) {
        cmdList.Get()->SetGraphicsRootDescriptorTable(7, cubemapArraySRV);
    }
}

void OpaqueRenderer::DrawInstanced(CommandList &cmdList, GeometryHandle geometryHandle,
                                   D3D12_GPU_VIRTUAL_ADDRESS instanceBufferAddress, uint32_t instanceCount,
                                   D3D12_GPU_DESCRIPTOR_HANDLE textureSRV) {
    if (!m_geometryManager) {
        OutputDebugStringW(L"[ERROR] OpaqueRenderer::DrawInstanced - GeometryResourceManager not set!\n");
        return;
    }

    // 从 GeometryResourceManager 获取几何体
    const TriangleMesh *mesh = m_geometryManager->GetGeometry<TriangleMesh>(geometryHandle);
    if (!mesh || !mesh->isGpuReady) {
        return;
    }

    auto &gpuMgr = GpuResourceManager::GetInstance();
    ID3D12Resource *vbResource = gpuMgr.GetResource(mesh->vertexBufferHandle);
    ID3D12Resource *ibResource = gpuMgr.GetResource(mesh->indexBufferHandle);

    if (!vbResource || !ibResource) {
        OutputDebugStringW(L"[ERROR] OpaqueRenderer::DrawInstanced - Invalid buffer!\n");
        return;
    }

    // 设置顶点/索引缓冲区
    D3D12_VERTEX_BUFFER_VIEW vbView;
    vbView.BufferLocation = vbResource->GetGPUVirtualAddress();
    vbView.StrideInBytes = mesh->vertexStride;
    vbView.SizeInBytes = static_cast<UINT>(mesh->vertexCount * mesh->vertexStride);
    cmdList.Get()->IASetVertexBuffers(0, 1, &vbView);

    D3D12_INDEX_BUFFER_VIEW ibView;
    ibView.BufferLocation = ibResource->GetGPUVirtualAddress();
    ibView.Format = mesh->indexFormat;
    ibView.SizeInBytes = static_cast<UINT>(mesh->indexCount * (mesh->indexFormat == DXGI_FORMAT_R32_UINT ? 4 : 2));
    cmdList.Get()->IASetIndexBuffer(&ibView);
    cmdList.Get()->IASetPrimitiveTopology(mesh->topology);

    // 绑定实例化数据 (slot 6, t12,space1) — 直接使用 GPU VA
    cmdList.Get()->SetGraphicsRootShaderResourceView(6, instanceBufferAddress);

    // 纹理 SRV (slot 3)
    if (textureSRV.ptr != 0) {
        cmdList.Get()->SetGraphicsRootDescriptorTable(3, textureSRV);
    }

    // 执行实例化绘制
    cmdList.Get()->DrawIndexedInstanced(mesh->indexCount, instanceCount, 0, 0, 0);
}

void OpaqueRenderer::EndFrame() {
    // 如果有需要每帧重置的状态，在此处处理
}

// ========================================================================
// 内部初始化
// ========================================================================

void OpaqueRenderer::LoadShaders() {

    UINT compileFlags = D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION | D3DCOMPILE_ENABLE_UNBOUNDED_DESCRIPTOR_TABLES;

    Microsoft::WRL::ComPtr<ID3DBlob> errors = nullptr;
    HRESULT hr;

    // VS — 统一实例化模式
    hr = D3DCompileFromFile(L"Shaders/color.hlsl", nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE, "VS", "vs_5_1",
                            compileFlags, 0, &m_vsBlob, &errors);

    if (FAILED(hr)) {
        if (errors) {
            const char *errMsg = reinterpret_cast<const char *>(errors->GetBufferPointer());
            OutputDebugStringA("=== VS COMPILATION ERROR ===\n");
            OutputDebugStringA(errMsg);
            OutputDebugStringA("============================\n");
        }
        throw std::runtime_error("OpaqueRenderer: Failed to compile Vertex Shader");
    }

    // PS
    errors = nullptr;
    hr = D3DCompileFromFile(L"Shaders/color.hlsl", nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE, "PS", "ps_5_1",
                            compileFlags, 0, &m_psBlob, &errors);

    if (FAILED(hr)) {
        if (errors) {
            const char *errMsg = reinterpret_cast<const char *>(errors->GetBufferPointer());
            OutputDebugStringA("=== PS COMPILATION ERROR ===\n");
            OutputDebugStringA(errMsg);
            OutputDebugStringA("============================\n");
        }
        throw std::runtime_error("OpaqueRenderer: Failed to compile Pixel Shader");
    }

    OutputDebugStringW(L"[INFO] Shaders compiled successfully at runtime\n");
}

void OpaqueRenderer::CreateRootSignature() {
    auto device = m_context->GetDevice();

    // ========================================================================
    // 根参数布局（统一实例化模式）:
    //   slot 0: b1 cbPass           (CBV)
    //   slot 1: b2 cbLights         (CBV)
    //   slot 2: t0,space1           StructuredBuffer<MaterialData> (SRV 描述符表)
    //   slot 3: t0                  纹理 SRV (描述符表)
    //   slot 4: t11,space1          StructuredBuffer<DirShadowData> (SRV 描述符表)
    //   slot 5: t14,space1          Texture2D 阴影贴图 (SRV 描述符表)
    //   slot 6: t12,space1          StructuredBuffer<InstanceData> (SRV)
    //   slot 7: t10                 TextureCubeArray 反射探针 Cubemap Array (SRV 描述符表)
    // ========================================================================
    CD3DX12_ROOT_PARAMETER slotRootParameter[8];

    CD3DX12_DESCRIPTOR_RANGE materialBufferRange;
    materialBufferRange.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 1, D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND);

    CD3DX12_DESCRIPTOR_RANGE texTable;
    texTable.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND);

    CD3DX12_DESCRIPTOR_RANGE shadowDataTable;
    shadowDataTable.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 11, 1, D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND);

    CD3DX12_DESCRIPTOR_RANGE shadowMapTable;
    shadowMapTable.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 14, 1, D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND);

    CD3DX12_DESCRIPTOR_RANGE cubemapTable;
    cubemapTable.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 10, 0, D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND);

    slotRootParameter[0].InitAsConstantBufferView(1, 0, D3D12_SHADER_VISIBILITY_ALL);                   // b1: cbPass
    slotRootParameter[1].InitAsConstantBufferView(2, 0, D3D12_SHADER_VISIBILITY_ALL);                   // b2: cbLights
    slotRootParameter[2].InitAsDescriptorTable(1, &materialBufferRange, D3D12_SHADER_VISIBILITY_PIXEL); // t0,space1
    slotRootParameter[3].InitAsDescriptorTable(1, &texTable, D3D12_SHADER_VISIBILITY_PIXEL);            // t0
    slotRootParameter[4].InitAsDescriptorTable(1, &shadowDataTable, D3D12_SHADER_VISIBILITY_PIXEL);     // t11,space1
    slotRootParameter[5].InitAsDescriptorTable(1, &shadowMapTable, D3D12_SHADER_VISIBILITY_PIXEL);      // t14,space1
    slotRootParameter[6].InitAsShaderResourceView(
        12, 1, D3D12_SHADER_VISIBILITY_ALL); // t12,space1: InstanceData StructuredBuffer
    slotRootParameter[7].InitAsDescriptorTable(1, &cubemapTable, D3D12_SHADER_VISIBILITY_PIXEL); // t10: Cubemap Array

    // ========================================================================
    // 静态采样器 (对齐 Common_PBR.hlsl: s0~s5 + s11)
    // ========================================================================
    CD3DX12_STATIC_SAMPLER_DESC staticSamplers[7];

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

    // s11: 阴影比较采样器
    staticSamplers[6].Init(11, D3D12_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT, D3D12_TEXTURE_ADDRESS_MODE_BORDER,
                           D3D12_TEXTURE_ADDRESS_MODE_BORDER, D3D12_TEXTURE_ADDRESS_MODE_BORDER, 0.0f, 0,
                           D3D12_COMPARISON_FUNC_LESS_EQUAL, D3D12_STATIC_BORDER_COLOR_OPAQUE_BLACK, 0.0f, 0.0f,
                           D3D12_SHADER_VISIBILITY_PIXEL);

    CD3DX12_ROOT_SIGNATURE_DESC rootSigDesc(8, slotRootParameter, 7, staticSamplers,
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

    // 输入布局对齐 GeometryGenerator::Vertex (44 bytes):
    //   Position(0,12) | Normal(12,12) | TangentU(24,12) | TexC(36,8)
    D3D12_INPUT_ELEMENT_DESC inputLayout[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"TANGENT", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 24, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 36, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0}};
    D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc = {};
    psoDesc.InputLayout = {inputLayout, _countof(inputLayout)};
    psoDesc.pRootSignature = m_rootSignature.Get();
    psoDesc.VS = {reinterpret_cast<BYTE *>(m_vsBlob->GetBufferPointer()), m_vsBlob->GetBufferSize()};
    psoDesc.PS = {reinterpret_cast<BYTE *>(m_psBlob->GetBufferPointer()), m_psBlob->GetBufferSize()};

    psoDesc.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
    psoDesc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);

    D3D12_DEPTH_STENCIL_DESC depthStencilDesc = {};
    depthStencilDesc.DepthEnable = TRUE;
    depthStencilDesc.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    depthStencilDesc.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
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

    OutputDebugStringW(L"[INFO] Opaque PSO created successfully\n");
}

} // namespace DX12Engine::Renderer
