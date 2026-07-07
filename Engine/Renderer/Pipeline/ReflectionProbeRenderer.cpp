#include "ReflectionProbeRenderer.h"
#include "Common/d3dUtil.h"
#include "Renderer/RHI/D3D12DeviceContext.h"
#include "Resource/GpuResourceManager.h"
#include "Resource/Manager/GeometryResourceManager.h"
#include "Renderer/Utils/ShaderUtils.h"

using namespace DX12Engine::Resource;

namespace DX12Engine::Renderer {

// ========================================================================
// 生命周期
// ========================================================================

void ReflectionProbeRenderer::SetDeviceContext(D3D12DeviceContext *context) { m_context = context; }

void ReflectionProbeRenderer::Initialize() {
    if (!m_context) {
        throw std::runtime_error("ReflectionProbeRenderer: Device context not set before Initialize");
    }

    LoadShaders();
    CreateRootSignature();
    CreatePSO();

    OutputDebugStringW(L"[INFO] ReflectionProbeRenderer initialized successfully (GS instanced)\n");
}

void ReflectionProbeRenderer::Shutdown() {
    m_pso.Reset();
    m_rootSignature.Reset();
    m_vsBlob.Reset();
    m_gsBlob.Reset();
    m_psBlob.Reset();
    m_inCapture = false;
    m_context = nullptr;
    m_geometryManager = nullptr;
    m_materialManager = nullptr;
}

// ========================================================================
// 着色器编译（VS + GS + PS）
// ========================================================================

void ReflectionProbeRenderer::LoadShaders() {
    if (!m_context) {
        throw std::runtime_error("ReflectionProbeRenderer: Device context not set for shader compilation");
    }

    UINT compileFlags = D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION | D3DCOMPILE_ENABLE_UNBOUNDED_DESCRIPTOR_TABLES;

    Microsoft::WRL::ComPtr<ID3DBlob> errors;
    HRESULT hr;

    hr = CompileShaderFromFile(L"Shaders/probe_capture.hlsl", "VS", "vs_5_1", compileFlags, m_vsBlob, &errors);
    if (FAILED(hr)) {
        if (errors) {
            OutputDebugStringA(static_cast<const char *>(errors->GetBufferPointer()));
        }
        throw std::runtime_error("ReflectionProbeRenderer: Failed to compile VS");
    }

    errors = nullptr;
    hr = CompileShaderFromFile(L"Shaders/probe_capture.hlsl", "GS", "gs_5_1", compileFlags, m_gsBlob, &errors);
    if (FAILED(hr)) {
        if (errors) {
            OutputDebugStringA(static_cast<const char *>(errors->GetBufferPointer()));
        }
        throw std::runtime_error("ReflectionProbeRenderer: Failed to compile GS");
    }

    errors = nullptr;
    hr = CompileShaderFromFile(L"Shaders/probe_capture.hlsl", "PS", "ps_5_1", compileFlags, m_psBlob, &errors);
    if (FAILED(hr)) {
        if (errors) {
            OutputDebugStringA(static_cast<const char *>(errors->GetBufferPointer()));
        }
        throw std::runtime_error("ReflectionProbeRenderer: Failed to compile PS");
    }
}

// ========================================================================
// 根签名（对齐 color.hlsl 的统一实例化模式，+ slot 5: b3 cbCapture）
// ========================================================================

void ReflectionProbeRenderer::CreateRootSignature() {
    auto device = m_context->GetDevice();

    // 根参数布局:
    //   slot 0: b1 cbPass           (CBV)
    //   slot 1: b2 cbLights         (CBV)
    //   slot 2: t0,space1           StructuredBuffer<MaterialData> (SRV 描述符表)
    //   slot 3: t0                  纹理 SRV (描述符表)
    //   slot 4: t12,space1          StructuredBuffer<InstanceData> (SRV)
    //   slot 5: b3 cbCapture        (CBV) — 6 面 VP + 探针位置
    CD3DX12_ROOT_PARAMETER slotRootParameter[6];

    CD3DX12_DESCRIPTOR_RANGE materialBufferRange;
    materialBufferRange.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 1);

    CD3DX12_DESCRIPTOR_RANGE texTable;
    texTable.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, UINT_MAX, 0, 2);

    slotRootParameter[0].InitAsConstantBufferView(1, 0);                 // b1: cbPass
    slotRootParameter[1].InitAsConstantBufferView(2, 0);                 // b2: cbLights
    slotRootParameter[2].InitAsDescriptorTable(1, &materialBufferRange); // t0,space1: MaterialData
    slotRootParameter[3].InitAsDescriptorTable(1, &texTable);            // t0: 纹理
    slotRootParameter[4].InitAsShaderResourceView(12, 1);                // t12,space1: InstanceData
    slotRootParameter[5].InitAsConstantBufferView(3, 0);                 // b3: cbCapture

    // 静态采样器（对齐 Common_PBR.hlsl）
    CD3DX12_STATIC_SAMPLER_DESC staticSamplers[8];
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
    staticSamplers[6].Init(10, D3D12_FILTER_MIN_MAG_MIP_LINEAR, D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
                           D3D12_TEXTURE_ADDRESS_MODE_CLAMP, D3D12_TEXTURE_ADDRESS_MODE_CLAMP);
    staticSamplers[7].Init(11, D3D12_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT, D3D12_TEXTURE_ADDRESS_MODE_BORDER,
                           D3D12_TEXTURE_ADDRESS_MODE_BORDER, D3D12_TEXTURE_ADDRESS_MODE_BORDER, 0.0f, 0,
                           D3D12_COMPARISON_FUNC_LESS_EQUAL, D3D12_STATIC_BORDER_COLOR_OPAQUE_BLACK, 0.0f, 0.0f,
                           D3D12_SHADER_VISIBILITY_PIXEL);

    CD3DX12_ROOT_SIGNATURE_DESC rootSigDesc(6, slotRootParameter, 8, staticSamplers,
                                            D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT);

    Microsoft::WRL::ComPtr<ID3DBlob> serializedRootSig;
    Microsoft::WRL::ComPtr<ID3DBlob> errorBlob;
    HRESULT hr =
        D3D12SerializeRootSignature(&rootSigDesc, D3D_ROOT_SIGNATURE_VERSION_1, &serializedRootSig, &errorBlob);
    if (errorBlob) {
        OutputDebugStringA(static_cast<const char *>(errorBlob->GetBufferPointer()));
    }
    ThrowIfFailed(hr);

    ThrowIfFailed(device->CreateRootSignature(0, serializedRootSig->GetBufferPointer(),
                                              serializedRootSig->GetBufferSize(), IID_PPV_ARGS(&m_rootSignature)));
}

// ========================================================================
// PSO — GS 实例化：输出到 Cubemap Array（R8G8B8A8_UNORM）+ D32_FLOAT
// ========================================================================

void ReflectionProbeRenderer::CreatePSO() {
    auto device = m_context->GetDevice();

    D3D12_INPUT_ELEMENT_DESC inputLayout[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"TANGENT", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 24, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 36, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
    };

    D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc = {};
    psoDesc.InputLayout = {inputLayout, _countof(inputLayout)};
    psoDesc.pRootSignature = m_rootSignature.Get();
    psoDesc.VS = {reinterpret_cast<BYTE *>(m_vsBlob->GetBufferPointer()), m_vsBlob->GetBufferSize()};
    psoDesc.GS = {reinterpret_cast<BYTE *>(m_gsBlob->GetBufferPointer()), m_gsBlob->GetBufferSize()};
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
    psoDesc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM; // Cubemap format
    psoDesc.DSVFormat = DXGI_FORMAT_D32_FLOAT;          // Shared depth buffer
    psoDesc.SampleDesc.Count = 1;
    psoDesc.SampleDesc.Quality = 0;

    ThrowIfFailed(device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&m_pso)));
}

// ========================================================================
// 探针捕获接口（GS 方案：一次 BeginCapture，GS 内部处理所有 6 面）
// ========================================================================

void ReflectionProbeRenderer::BeginCapture(CommandList &cmdList, ID3D12Resource *cubemapResource,
                                           D3D12_CPU_DESCRIPTOR_HANDLE cubemapRTV, D3D12_CPU_DESCRIPTOR_HANDLE depthDSV,
                                           uint32_t faceWidth, uint32_t faceHeight,
                                           D3D12_GPU_VIRTUAL_ADDRESS captureCBAddress,
                                           D3D12_GPU_VIRTUAL_ADDRESS lightCBAddress,
                                           D3D12_GPU_DESCRIPTOR_HANDLE materialBufferSRV,
                                           D3D12_GPU_DESCRIPTOR_HANDLE textureHeapStart) {
    if (!m_pso || !m_rootSignature) {
        ErrorReporter::Report("ReflectionProbeRenderer: PSO or RootSignature not initialized");
        return;
    }

    m_faceWidth = faceWidth;
    m_faceHeight = faceHeight;
    m_inCapture = true;
    m_captureResource = cubemapResource;

    // 将 Cubemap 从 COMMON 转换为 RENDER_TARGET 状态
    if (cubemapResource) {
        D3D12_RESOURCE_BARRIER barrier = {};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = cubemapResource;
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        cmdList.Get()->ResourceBarrier(1, &barrier);
    }

    // 设置视口/裁剪
    D3D12_VIEWPORT viewport = {0.0f, 0.0f, static_cast<float>(faceWidth), static_cast<float>(faceHeight), 0.0f, 1.0f};
    D3D12_RECT scissorRect = {0, 0, static_cast<LONG>(faceWidth), static_cast<LONG>(faceHeight)};
    cmdList.Get()->RSSetViewports(1, &viewport);
    cmdList.Get()->RSSetScissorRects(1, &scissorRect);

    // 绑定 PSO 和根签名
    cmdList.Get()->SetPipelineState(m_pso.Get());
    cmdList.Get()->SetGraphicsRootSignature(m_rootSignature.Get());

    // 绑定常量和材质 SRV
    cmdList.Get()->SetGraphicsRootConstantBufferView(0, 0); // b1: cbPass (unused by probe shader)
    if (lightCBAddress)
        cmdList.Get()->SetGraphicsRootConstantBufferView(1, lightCBAddress); // b2: cbLights
    if (materialBufferSRV.ptr)
        cmdList.Get()->SetGraphicsRootDescriptorTable(2, materialBufferSRV); // t0,space1: MaterialData

    // 绑定纹理堆 (slot 3, t0,space0)
    if (textureHeapStart.ptr)
        cmdList.Get()->SetGraphicsRootDescriptorTable(3, textureHeapStart);

    if (captureCBAddress)
        cmdList.Get()->SetGraphicsRootConstantBufferView(5, captureCBAddress); // b3: cbCapture

    // 设置渲染目标并清除（GS 通过 SV_RenderTargetArrayIndex 写入各面）
    cmdList.Get()->OMSetRenderTargets(1, &cubemapRTV, FALSE, &depthDSV);
    const float clearColor[4] = {0.2f, 0.2f, 0.3f, 1.0f};
    cmdList.Get()->ClearRenderTargetView(cubemapRTV, clearColor, 0, nullptr);
    cmdList.Get()->ClearDepthStencilView(depthDSV, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
}

void ReflectionProbeRenderer::EndCapture(CommandList &cmdList) {
    if (m_inCapture && m_captureResource) {
        D3D12_RESOURCE_BARRIER barrier = {};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = m_captureResource;
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        cmdList.Get()->ResourceBarrier(1, &barrier);
        m_captureResource = nullptr;
    }
    m_inCapture = false;
}

// ========================================================================
// 统一实例化绘制
// ========================================================================

void ReflectionProbeRenderer::DrawInstanced(CommandList &cmdList, GeometryHandle geometryHandle,
                                            D3D12_GPU_VIRTUAL_ADDRESS instanceBufferAddress, uint32_t instanceCount) {
    if (!m_inCapture || !m_geometryManager)
        return;

    // 绑定实例数据 SRV
    cmdList.Get()->SetGraphicsRootShaderResourceView(4, instanceBufferAddress);

    // 纹理数组已在 BeginCapture 全局绑定 (slot 3)

    // 获取顶点/索引缓冲区并绘制
    auto *mesh = m_geometryManager->GetGeometry<TriangleMesh>(geometryHandle);
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
    cmdList.Get()->IASetVertexBuffers(0, 1, &vbView);

    D3D12_INDEX_BUFFER_VIEW ibView;
    ibView.BufferLocation = ibResource->GetGPUVirtualAddress();
    ibView.Format = mesh->indexFormat;
    ibView.SizeInBytes = static_cast<UINT>(mesh->indexCount * (mesh->indexFormat == DXGI_FORMAT_R32_UINT ? 4 : 2));
    cmdList.Get()->IASetIndexBuffer(&ibView);
    cmdList.Get()->IASetPrimitiveTopology(mesh->topology);
    cmdList.Get()->DrawIndexedInstanced(mesh->indexCount, instanceCount, 0, 0, 0);
}

} // namespace DX12Engine::Renderer
