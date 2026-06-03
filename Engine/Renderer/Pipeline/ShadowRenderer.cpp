#include "ShadowRenderer.h"
#include "Common/d3dUtil.h"
#include "Renderer/RHI/D3D12DeviceContext.h"
#include "Renderer/Scene/LightManager/LightManager.h"
#include "Resource/Core/DescriptorHeapCollection.h"
#include "Resource/Geometry/TriangleMesh.h"
#include "Resource/GpuResourceManager.h"
#include "Resource/Manager/GeometryResourceManager.h"
#include "Resource/Struct/Descriptor.h"
#include <DirectXMath.h>
#include <d3dcompiler.h>

using namespace DirectX;
using namespace DX12Engine::Renderer;
using namespace DX12Engine::Resource;

namespace DX12Engine::Renderer {

// ============================================================================
// 生命周期管理
// ============================================================================

void ShadowRenderer::SetDeviceContext(D3D12DeviceContext *context) { m_context = context; }

void ShadowRenderer::Initialize() {
    if (!m_context) {
        throw std::runtime_error("ShadowRenderer: Device context not set before Initialize");
    }

    LoadShaders();

    // 暂时不处理点光源阴影
    if (!m_dirVSBlob || !m_spotVSBlob || !m_psBlob) {
        OutputDebugStringW(L"[ERROR] ShadowRenderer: Failed to load shaders!\n");
        throw std::runtime_error("ShadowRenderer: Failed to load shaders");
    }

    CreateRootSignatures();
    CreatePSOs();

    OutputDebugStringW(L"[INFO] ShadowRenderer initialized successfully\n");
}

void ShadowRenderer::OnResize(uint32_t width, uint32_t height) {
    // ShadowRenderer 不依赖屏幕分辨率
    (void)width;
    (void)height;
}

void ShadowRenderer::Update(float deltaTime) { (void)deltaTime; }

void ShadowRenderer::EndFrame() {}

// ============================================================================
// 阴影渲染接口
// ============================================================================

void ShadowRenderer::RenderDirectionalShadow(CommandList &cmdList, D3D12_GPU_VIRTUAL_ADDRESS shadowCBAddress,
                                             const DirShadowResources &shadowRes) {
    if (!m_dirShadowPSO || !shadowRes.isValid || !m_descriptorHeaps)
        return;

    auto &gpuMgr = GpuResourceManager::GetInstance();
    ID3D12Resource *depthResource = gpuMgr.GetResource(shadowRes.textureHandle);
    if (!depthResource)
        return;

    // 1. 过渡到 DEPTH_WRITE（资源初始状态为 SRV 或 EndShadowPass 后为 SRV）
    CD3DX12_RESOURCE_BARRIER barrier = CD3DX12_RESOURCE_BARRIER::Transition(
        depthResource, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
        D3D12_RESOURCE_STATE_DEPTH_WRITE);
    cmdList.Get()->ResourceBarrier(1, &barrier);

    // 2. 清除深度
    D3D12_CPU_DESCRIPTOR_HANDLE dsvHandle = m_descriptorHeaps->GetCpuHandle(DescriptorHeapType::Dsv, shadowRes.dsvSlot);
    cmdList.Get()->ClearDepthStencilView(dsvHandle, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);

    // 3. 设置渲染目标
    cmdList.Get()->OMSetRenderTargets(0, nullptr, FALSE, &dsvHandle);

    // 4. 设置视口/裁剪矩形
    D3D12_VIEWPORT viewport = {
        0.0f, 0.0f, static_cast<float>(shadowRes.resolution), static_cast<float>(shadowRes.resolution), 0.0f, 1.0f};
    D3D12_RECT scissor = {0, 0, static_cast<LONG>(shadowRes.resolution), static_cast<LONG>(shadowRes.resolution)};
    cmdList.Get()->RSSetViewports(1, &viewport);
    cmdList.Get()->RSSetScissorRects(1, &scissor);

    // 5. 设置 PSO + 根签名
    cmdList.Get()->SetPipelineState(m_dirShadowPSO.Get());
    cmdList.Get()->SetGraphicsRootSignature(m_rootSignature.Get());

    // 6. 设置光源 VP 常量 (b1, CBV)
    cmdList.Get()->SetGraphicsRootConstantBufferView(1, shadowCBAddress);
}

void ShadowRenderer::RenderPointShadow(CommandList &cmdList, D3D12_GPU_VIRTUAL_ADDRESS shadowCBAddress,
                                       const PointShadowResources &shadowRes) {
    if (!m_pointShadowPSO || !shadowRes.isValid || !m_descriptorHeaps)
        return;

    auto &gpuMgr = GpuResourceManager::GetInstance();
    ID3D12Resource *depthResource = gpuMgr.GetResource(shadowRes.textureHandle);
    if (!depthResource)
        return;

    // 1. 过渡到 DEPTH_WRITE
    CD3DX12_RESOURCE_BARRIER barrier = CD3DX12_RESOURCE_BARRIER::Transition(
        depthResource, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
        D3D12_RESOURCE_STATE_DEPTH_WRITE);
    cmdList.Get()->ResourceBarrier(1, &barrier);

    // 2. 清除深度
    D3D12_CPU_DESCRIPTOR_HANDLE dsvHandle = m_descriptorHeaps->GetCpuHandle(DescriptorHeapType::Dsv, shadowRes.dsvSlot);
    cmdList.Get()->ClearDepthStencilView(dsvHandle, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);

    // 3. 设置渲染目标 (无颜色目标)
    cmdList.Get()->OMSetRenderTargets(0, nullptr, FALSE, &dsvHandle);

    // 4. 设置视口/裁剪矩形
    D3D12_VIEWPORT viewport = {
        0.0f, 0.0f, static_cast<float>(shadowRes.resolution), static_cast<float>(shadowRes.resolution), 0.0f, 1.0f};
    D3D12_RECT scissor = {0, 0, static_cast<LONG>(shadowRes.resolution), static_cast<LONG>(shadowRes.resolution)};
    cmdList.Get()->RSSetViewports(1, &viewport);
    cmdList.Get()->RSSetScissorRects(1, &scissor);

    // 5. 设置 PSO + 根签名
    cmdList.Get()->SetPipelineState(m_pointShadowPSO.Get());
    cmdList.Get()->SetGraphicsRootSignature(m_rootSignature.Get());

    // 6. 设置光源 VP 常量 (b1, CBV) — 6 个面的 VP 矩阵
    cmdList.Get()->SetGraphicsRootConstantBufferView(1, shadowCBAddress);
}

void ShadowRenderer::RenderSpotShadow(CommandList &cmdList, D3D12_GPU_VIRTUAL_ADDRESS shadowCBAddress,
                                      const SpotShadowResources &shadowRes) {
    if (!m_spotShadowPSO || !shadowRes.isValid || !m_descriptorHeaps)
        return;

    auto &gpuMgr = GpuResourceManager::GetInstance();
    ID3D12Resource *depthResource = gpuMgr.GetResource(shadowRes.textureHandle);
    if (!depthResource)
        return;

    // 1. 过渡到 DEPTH_WRITE
    CD3DX12_RESOURCE_BARRIER barrier = CD3DX12_RESOURCE_BARRIER::Transition(
        depthResource, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
        D3D12_RESOURCE_STATE_DEPTH_WRITE);
    cmdList.Get()->ResourceBarrier(1, &barrier);

    // 2. 清除深度
    D3D12_CPU_DESCRIPTOR_HANDLE dsvHandle = m_descriptorHeaps->GetCpuHandle(DescriptorHeapType::Dsv, shadowRes.dsvSlot);
    cmdList.Get()->ClearDepthStencilView(dsvHandle, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);

    // 3. 设置渲染目标
    cmdList.Get()->OMSetRenderTargets(0, nullptr, FALSE, &dsvHandle);

    // 4. 设置视口/裁剪矩形
    D3D12_VIEWPORT viewport = {
        0.0f, 0.0f, static_cast<float>(shadowRes.resolution), static_cast<float>(shadowRes.resolution), 0.0f, 1.0f};
    D3D12_RECT scissor = {0, 0, static_cast<LONG>(shadowRes.resolution), static_cast<LONG>(shadowRes.resolution)};
    cmdList.Get()->RSSetViewports(1, &viewport);
    cmdList.Get()->RSSetScissorRects(1, &scissor);

    // 5. 设置 PSO + 根签名
    cmdList.Get()->SetPipelineState(m_spotShadowPSO.Get());
    cmdList.Get()->SetGraphicsRootSignature(m_rootSignature.Get());

    // 6. 设置光源 VP 常量 (b1, CBV)
    cmdList.Get()->SetGraphicsRootConstantBufferView(1, shadowCBAddress);
}

void ShadowRenderer::DrawShadowMesh(CommandList &cmdList, GeometryHandle geometryHandle,
                                    const DirectX::XMMATRIX &worldMatrix, D3D12_GPU_VIRTUAL_ADDRESS objectCBAddress) {
    if (!m_geometryManager)
        return;

    const TriangleMesh *mesh = m_geometryManager->GetTriangleMesh(geometryHandle);
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

    // b0: ShadowObjectConstants (World 矩阵)
    cmdList.Get()->SetGraphicsRootConstantBufferView(0, objectCBAddress);

    cmdList.Get()->DrawIndexedInstanced(mesh->indexCount, 1, 0, 0, 0);
}

void ShadowRenderer::EndShadowPass(CommandList &cmdList, GpuResourceHandle textureHandle) {
    auto &gpuMgr = GpuResourceManager::GetInstance();
    ID3D12Resource *depthResource = gpuMgr.GetResource(textureHandle);
    if (!depthResource)
        return;

    // 将深度资源从 DEPTH_WRITE 恢复到 SRV 状态，供主 Pass 采样阴影贴图
    CD3DX12_RESOURCE_BARRIER barrier = CD3DX12_RESOURCE_BARRIER::Transition(
        depthResource, D3D12_RESOURCE_STATE_DEPTH_WRITE,
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    cmdList.Get()->ResourceBarrier(1, &barrier);
}

// ============================================================================
// 内部初始化
// ============================================================================

void ShadowRenderer::LoadShaders() {
    UINT compileFlags = D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
    compileFlags |= D3DCOMPILE_ENABLE_UNBOUNDED_DESCRIPTOR_TABLES;
    Microsoft::WRL::ComPtr<ID3DBlob> errors = nullptr;
    HRESULT hr;

    // 方向光 VS
    hr = D3DCompileFromFile(L"Shaders/Shadow.hlsl", nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE, "DirShadowVS", "vs_5_1",
                            compileFlags, 0, &m_dirVSBlob, &errors);
    if (FAILED(hr)) {
        if (errors) {
            OutputDebugStringA("=== DirShadowVS ERROR ===\n");
            OutputDebugStringA(reinterpret_cast<const char *>(errors->GetBufferPointer()));
            OutputDebugStringA("=========================\n");
        }
        throw std::runtime_error("ShadowRenderer: Failed to compile DirShadowVS");
    }

    // // 点光源 VS
    // errors = nullptr;
    // hr = D3DCompileFromFile(L"Shaders/Shadow.hlsl", nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE, "PointShadowVS",
    //                         "vs_5_1", compileFlags, 0, &m_pointVSBlob, &errors);
    // if (FAILED(hr)) {
    //     if (errors) {
    //         OutputDebugStringA("=== PointShadowVS ERROR ===\n");
    //         OutputDebugStringA(reinterpret_cast<const char *>(errors->GetBufferPointer()));
    //         OutputDebugStringA("===========================\n");
    //     }
    //     throw std::runtime_error("ShadowRenderer: Failed to compile PointShadowVS");
    // }

    // // 点光源 GS
    // errors = nullptr;
    // hr = D3DCompileFromFile(L"Shaders/Shadow.hlsl", nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE, "PointShadowGS",
    //                         "gs_5_1", compileFlags, 0, &m_pointGSBlob, &errors);
    // if (FAILED(hr)) {
    //     if (errors) {
    //         OutputDebugStringA("=== PointShadowGS ERROR ===\n");
    //         OutputDebugStringA(reinterpret_cast<const char *>(errors->GetBufferPointer()));
    //         OutputDebugStringA("===========================\n");
    //     }
    //     throw std::runtime_error("ShadowRenderer: Failed to compile PointShadowGS");
    // }

    // 聚光灯 VS
    errors = nullptr;
    hr = D3DCompileFromFile(L"Shaders/Shadow.hlsl", nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE, "SpotShadowVS",
                            "vs_5_1", compileFlags, 0, &m_spotVSBlob, &errors);
    if (FAILED(hr)) {
        if (errors) {
            OutputDebugStringA("=== SpotShadowVS ERROR ===\n");
            OutputDebugStringA(reinterpret_cast<const char *>(errors->GetBufferPointer()));
            OutputDebugStringA("==========================\n");
        }
        throw std::runtime_error("ShadowRenderer: Failed to compile SpotShadowVS");
    }

    // 阴影 PS（方向光/聚光灯共用，VertexOut 输入）
    errors = nullptr;
    hr = D3DCompileFromFile(L"Shaders/Shadow.hlsl", nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE, "ShadowPS", "ps_5_1",
                            compileFlags, 0, &m_psBlob, &errors);
    if (FAILED(hr)) {
        if (errors) {
            OutputDebugStringA("=== ShadowPS ERROR ===\n");
            OutputDebugStringA(reinterpret_cast<const char *>(errors->GetBufferPointer()));
            OutputDebugStringA("======================\n");
        }
        throw std::runtime_error("ShadowRenderer: Failed to compile ShadowPS");
    }

    // 阴影 PS（点光源，GeoOut 输入含 SV_RenderTargetArrayIndex）
    errors = nullptr;
    hr = D3DCompileFromFile(L"Shaders/Shadow.hlsl", nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE, "ShadowPS_Point",
                            "ps_5_1", compileFlags, 0, &m_psPointBlob, &errors);
    if (FAILED(hr)) {
        if (errors) {
            OutputDebugStringA("=== ShadowPS_Point ERROR ===\n");
            OutputDebugStringA(reinterpret_cast<const char *>(errors->GetBufferPointer()));
            OutputDebugStringA("===========================\n");
        }
        throw std::runtime_error("ShadowRenderer: Failed to compile ShadowPS_Point");
    }

    OutputDebugStringW(L"[INFO] Shadow shaders compiled successfully\n");
}

void ShadowRenderer::CreateRootSignatures() {
    auto device = m_context->GetDevice();

    // ========================================================================
    // 根参数布局 (对齐 Shadow.hlsl):
    //   slot 0: b0 cbShadowObject  (CBV — 物体 World 矩阵)
    //   slot 1: b1 cbDirShadow / cbPointShadow / cbSpotShadow (CBV)
    // ========================================================================
    CD3DX12_ROOT_PARAMETER slotRootParameter[2];

    slotRootParameter[0].InitAsConstantBufferView(0, 0, D3D12_SHADER_VISIBILITY_ALL); // b0: cbShadowObject
    slotRootParameter[1].InitAsConstantBufferView(1, 0, D3D12_SHADER_VISIBILITY_ALL); // b1: 阴影常量 (CBV)

    CD3DX12_ROOT_SIGNATURE_DESC rootSigDesc(2, slotRootParameter, 0, nullptr,
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

void ShadowRenderer::CreatePSOs() {
    auto device = m_context->GetDevice();

    // 输入布局：只需要 POSITION
    D3D12_INPUT_ELEMENT_DESC inputLayout[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0}};

    // ========================================================================
    // 深度模板状态（写深度，无模板测试）
    // ========================================================================
    D3D12_DEPTH_STENCIL_DESC depthStencilDesc = {};
    depthStencilDesc.DepthEnable = TRUE;
    depthStencilDesc.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    depthStencilDesc.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
    depthStencilDesc.StencilEnable = FALSE;

    // ========================================================================
    // 光栅化状态（增加深度偏移减少阴影痤疮）
    // ========================================================================
    D3D12_RASTERIZER_DESC rasterizerDesc = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
    rasterizerDesc.DepthBias = 1000;
    rasterizerDesc.DepthBiasClamp = 0.0f;
    rasterizerDesc.SlopeScaledDepthBias = 1.0f;
    rasterizerDesc.CullMode = D3D12_CULL_MODE_BACK; // 默认背面剔除，可切换

    // ========================================================================
    // PSO 基础模板
    // ========================================================================
    D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc = {};
    psoDesc.InputLayout = {inputLayout, _countof(inputLayout)};
    psoDesc.pRootSignature = m_rootSignature.Get();
    psoDesc.RasterizerState = rasterizerDesc;
    psoDesc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
    psoDesc.DepthStencilState = depthStencilDesc;
    psoDesc.SampleMask = UINT_MAX;
    psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    psoDesc.NumRenderTargets = 0; // 无颜色输出
    psoDesc.RTVFormats[0] = DXGI_FORMAT_UNKNOWN;
    psoDesc.DSVFormat = DXGI_FORMAT_D32_FLOAT;
    psoDesc.SampleDesc.Count = 1;
    psoDesc.SampleDesc.Quality = 0;

    // ------------------------------------------------------------------
    // 方向光阴影 PSO (VS + PS，无 GS)
    // ------------------------------------------------------------------
    psoDesc.VS = {reinterpret_cast<BYTE *>(m_dirVSBlob->GetBufferPointer()), m_dirVSBlob->GetBufferSize()};
    psoDesc.PS = {reinterpret_cast<BYTE *>(m_psBlob->GetBufferPointer()), m_psBlob->GetBufferSize()};
    psoDesc.GS = {nullptr, 0};
    ThrowIfFailed(device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&m_dirShadowPSO)));

    // // ------------------------------------------------------------------
    // // 点光源阴影 PSO (VS + GS + PS，GS 输出到 6 个 cube 面)
    // // ------------------------------------------------------------------
    // psoDesc.VS = {reinterpret_cast<BYTE *>(m_pointVSBlob->GetBufferPointer()), m_pointVSBlob->GetBufferSize()};
    // psoDesc.GS = {reinterpret_cast<BYTE *>(m_pointGSBlob->GetBufferPointer()), m_pointGSBlob->GetBufferSize()};
    // psoDesc.PS = {reinterpret_cast<BYTE *>(m_psPointBlob->GetBufferPointer()), m_psPointBlob->GetBufferSize()};
    // ThrowIfFailed(device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&m_pointShadowPSO)));

    // ------------------------------------------------------------------
    // 聚光灯阴影 PSO (VS + PS，无 GS)
    // ------------------------------------------------------------------
    psoDesc.VS = {reinterpret_cast<BYTE *>(m_spotVSBlob->GetBufferPointer()), m_spotVSBlob->GetBufferSize()};
    psoDesc.GS = {nullptr, 0};
    psoDesc.PS = {reinterpret_cast<BYTE *>(m_psBlob->GetBufferPointer()), m_psBlob->GetBufferSize()};
    ThrowIfFailed(device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&m_spotShadowPSO)));

    OutputDebugStringW(L"[INFO] Shadow PSOs created successfully\n");
}

} // namespace DX12Engine::Renderer
