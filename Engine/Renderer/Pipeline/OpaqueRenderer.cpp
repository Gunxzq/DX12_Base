#include "OpaqueRenderer.h"
#include "Common/d3dUtil.h"
#include "ECS/Core/Components.h"
#include "Renderer/RHI/D3D12DeviceContext.h"
#include "Renderer/Utils/GeometryGenerator.h"
#include "Resource/GpuResourceManager.h"
#include <DirectXMath.h>
#include <d3dcompiler.h>
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
    m_objectCBAlignedSize = d3dUtil::CalcConstantBufferByteSize(sizeof(ObjectConstants));

    for (uint32_t i = 0; i < 3; ++i) {
        UINT numObjectsPerFrame = 1000;
        UINT cbSize = m_objectCBAlignedSize * numObjectsPerFrame;

        ThrowIfFailed(d3dUtil::CreateUploadBuffer(device, cbSize, D3D12_RESOURCE_STATE_GENERIC_READ,
                                                  &m_frameResources[i].objectConstantBuffer));
        m_frameResources[i].objectConstantBuffer->Map(0, nullptr, &m_frameResources[i].mappedData);
    }

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

void OpaqueRenderer::BeginFrame(CommandList &cmdList, uint32_t backBufferIndex,
                                D3D12_GPU_VIRTUAL_ADDRESS passConstantsAddress) {
    if (!m_pso || !m_rootSignature)
        return;

    // 重置当前帧的 Object CB 偏移量
    m_currentObjectCBOffset = 0;

    cmdList.Get()->SetPipelineState(m_pso.Get());
    cmdList.Get()->SetGraphicsRootSignature(m_rootSignature.Get());

    cmdList.Get()->SetGraphicsRootConstantBufferView(1, passConstantsAddress);
}

void OpaqueRenderer::DrawMesh(CommandList &cmdList, const MeshComponent &mesh, const TransformComponent &transform,
                              uint32_t backBufferIndex) {

    static int drawCallCount = 0;
    drawCallCount++;
    char buf[128];
    sprintf_s(buf, "[DEBUG] DrawMesh #%d called, indexCount=%u\n", drawCallCount, mesh.indexCount);
    OutputDebugStringA(buf);

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

    // 3. 构建 ObjectConstants (World Matrix)
    XMMATRIX translation = XMMatrixTranslation(transform.position.x, transform.position.y, transform.position.z);
    XMMATRIX rotation = XMMatrixRotationRollPitchYaw(transform.rotation.x, transform.rotation.y, transform.rotation.z);
    XMMATRIX scale = XMMatrixScaling(transform.scale.x, transform.scale.y, transform.scale.z);
    XMMATRIX world = scale * rotation * translation;

    // 计算 WorldInvTranspose (用于法线，虽然当前 Shader 没用法线，但结构体里有)
    XMMATRIX worldInvTranspose = XMMatrixTranspose(XMMatrixInverse(nullptr, world));

    ObjectConstants objCB;
    XMStoreFloat4x4(&objCB.World, world);
    XMStoreFloat4x4(&objCB.WorldInvTranspose, worldInvTranspose);

    if (drawCallCount <= 5) { // 只打印前5帧，避免刷屏
        wchar_t buf[512];
        swprintf_s(buf,
                   L"[DEBUG] DrawMesh #%d | Pos(%.1f, %.1f, %.1f)\n"
                   L"  World[0]: %.4f, %.4f, %.4f, %.4f\n"
                   L"  World[1]: %.4f, %.4f, %.4f, %.4f\n"
                   L"  World[2]: %.4f, %.4f, %.4f, %.4f\n"
                   L"  World[3]: %.4f, %.4f, %.4f, %.4f\n",
                   drawCallCount, transform.position.x, transform.position.y, transform.position.z, objCB.World._11,
                   objCB.World._12, objCB.World._13, objCB.World._14, objCB.World._21, objCB.World._22, objCB.World._23,
                   objCB.World._24, objCB.World._31, objCB.World._32, objCB.World._33, objCB.World._34, objCB.World._41,
                   objCB.World._42, objCB.World._43, objCB.World._44);
        OutputDebugStringW(buf);
    }
    // ==================================

    // 4. 上传 ObjectConstants 到当前帧的环形缓冲区
    // 计算当前物体的偏移地址
    uint8_t *mappedBase = static_cast<uint8_t *>(m_frameResources[backBufferIndex].mappedData);
    uint8_t *mappedCurrent = mappedBase + m_currentObjectCBOffset;

    // 拷贝数据
    memcpy(mappedCurrent, &objCB, sizeof(ObjectConstants));

    // 计算当前物体的 GPU 虚拟地址
    D3D12_GPU_VIRTUAL_ADDRESS objCBAddress =
        m_frameResources[backBufferIndex].objectConstantBuffer->GetGPUVirtualAddress() + m_currentObjectCBOffset;

    // 5. 绑定 Object Constant Buffer (b0)
    cmdList.Get()->SetGraphicsRootConstantBufferView(0, objCBAddress);

    // 6. 绘制调用
    cmdList.Get()->DrawIndexedInstanced(mesh.indexCount, 1, 0, 0, 0);

    // 7. 更新偏移量，为下一个物体做准备
    m_currentObjectCBOffset += m_objectCBAlignedSize;
}

void OpaqueRenderer::EndFrame() {
    // 如果有需要每帧重置的状态，在此处处理
}

// ========================================================================
// 内部初始化
// ========================================================================

void OpaqueRenderer::LoadShaders() {

    UINT compileFlags = D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;

    Microsoft::WRL::ComPtr<ID3DBlob> errors = nullptr;
    HRESULT hr;

    hr = D3DCompileFromFile(L"Shaders/color.hlsl", // 文件名
                            nullptr,               // defines
                            nullptr,               // includes
                            "VS",                  // entry point
                            "vs_5_1",              // target profile
                            compileFlags,          // flags1
                            0,                     // flags2
                            &m_vsBlob,             // output shader blob
                            &errors                // error messages
    );

    if (FAILED(hr)) {
        if (errors) {
            OutputDebugStringA(reinterpret_cast<const char *>(errors->GetBufferPointer()));
        }
        throw std::runtime_error("OpaqueRenderer: Failed to compile Vertex Shader");
    }

    // 2. 编译像素着色器
    errors = nullptr; // 重置错误 Blob
    hr = D3DCompileFromFile(L"Shaders/color.hlsl", nullptr, nullptr, "PS", "ps_5_1", compileFlags, 0, &m_psBlob,
                            &errors);

    if (FAILED(hr)) {
        if (errors) {
            OutputDebugStringA(reinterpret_cast<const char *>(errors->GetBufferPointer()));
        }
        throw std::runtime_error("OpaqueRenderer: Failed to compile Pixel Shader");
    }

    OutputDebugStringW(L"[INFO] Shaders compiled successfully at runtime\n");
}

void OpaqueRenderer::CreateRootSignature() {
    auto device = m_context->GetDevice();

    CD3DX12_ROOT_PARAMETER slotRootParameter[2];
    slotRootParameter[0].InitAsConstantBufferView(0); // b0: cbPerObject
    slotRootParameter[1].InitAsConstantBufferView(1); // b1: cbPass

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

    // // // 启用线框模式以便调试
    // D3D12_RASTERIZER_DESC rasterizerDesc = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
    // rasterizerDesc.FillMode = D3D12_FILL_MODE_WIREFRAME;
    // rasterizerDesc.CullMode = D3D12_CULL_MODE_NONE; // 禁用背面剔除
    // psoDesc.RasterizerState = rasterizerDesc;

    psoDesc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);

    psoDesc.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);

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

} // namespace DX12Engine::Renderer