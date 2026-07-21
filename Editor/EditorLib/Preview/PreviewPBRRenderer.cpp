#include "PreviewPBRRenderer.h"
#include "Common/Common.h"
#include "Renderer/RHI/D3D12DeviceContext.h"
#include "Renderer/Utils/GeometryGenerator.h"
#include "Resource/GpuResourceManager.h"
#include "Resource/Manager/GeometryResourceManager.h"
#include <DirectXMath.h>
#include <vector>

using namespace DX12Engine;
using namespace DX12Engine::Renderer;
using namespace DirectX;



// ========================================================================
// IRenderer 生命周期
// ========================================================================

void PreviewPBRRenderer::SetDeviceContext(D3D12DeviceContext *context) { m_context = context; }

void PreviewPBRRenderer::Initialize() {
    if (!m_context) {
        ErrorReporter::Report("PreviewPBRRenderer: DeviceContext not set");
        return;
    }
    LoadShaders();
    CreateRootSignature();
    CreatePSO();
}

void PreviewPBRRenderer::OnResize(uint32_t, uint32_t) {}
void PreviewPBRRenderer::Update(float) {}
void PreviewPBRRenderer::EndFrame() {}

// ========================================================================
// 内部初始化
// ========================================================================

void PreviewPBRRenderer::LoadShaders() {
    m_vs = d3dUtil::CompileShader(L"Shaders/preview.hlsl", nullptr, "VS", "vs_5_1");
    m_ps = d3dUtil::CompileShader(L"Shaders/preview.hlsl", nullptr, "PS", "ps_5_1");
    // Unlit 纹理预览着色器
    m_vsUnlit = d3dUtil::CompileShader(L"Shaders/preview_unlit.hlsl", nullptr, "VS", "vs_5_1");
    m_psUnlit = d3dUtil::CompileShader(L"Shaders/preview_unlit.hlsl", nullptr, "PS", "ps_5_1");
}

void PreviewPBRRenderer::CreateRootSignature() {
    auto *device = m_context ? m_context->GetDevice() : nullptr;
    if (!device) return;

    // slot 0: CBV b0 (PreviewCB: WVP + World + Camera + Light + Material)
    // slot 1: SRV t0 (预览纹理, 描述符表)
    CD3DX12_ROOT_PARAMETER params[2] = {};
    params[0].InitAsConstantBufferView(0, 0, D3D12_SHADER_VISIBILITY_ALL);

    CD3DX12_DESCRIPTOR_RANGE texRange;
    texRange.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0);
    params[1].InitAsDescriptorTable(1, &texRange, D3D12_SHADER_VISIBILITY_PIXEL);

    CD3DX12_STATIC_SAMPLER_DESC sampler(0, D3D12_FILTER_MIN_MAG_MIP_LINEAR,
                                        D3D12_TEXTURE_ADDRESS_MODE_WRAP,
                                        D3D12_TEXTURE_ADDRESS_MODE_WRAP,
                                        D3D12_TEXTURE_ADDRESS_MODE_WRAP);

    CD3DX12_ROOT_SIGNATURE_DESC rsDesc;
    rsDesc.Init(2, params, 1, &sampler,
                D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT);

    Microsoft::WRL::ComPtr<ID3DBlob> sBlob, errBlob;
    if (FAILED(D3D12SerializeRootSignature(&rsDesc, D3D_ROOT_SIGNATURE_VERSION_1, &sBlob, &errBlob))) {
        const char *msg = errBlob ? (const char *)errBlob->GetBufferPointer() : "unknown";
        ErrorReporter::Report("PreviewPBRRenderer: SerializeRootSignature failed: %s", msg);
        return;
    }
    device->CreateRootSignature(0, sBlob->GetBufferPointer(), sBlob->GetBufferSize(),
                                IID_PPV_ARGS(&m_rootSignature));
}

void PreviewPBRRenderer::CreatePSO() {
    auto *device = m_context ? m_context->GetDevice() : nullptr;
    if (!device || !m_rootSignature || !m_vs || !m_ps) return;

    D3D12_GRAPHICS_PIPELINE_STATE_DESC desc = {};
    desc.pRootSignature = m_rootSignature.Get();
    desc.VS = {m_vs->GetBufferPointer(), m_vs->GetBufferSize()};
    desc.PS = {m_ps->GetBufferPointer(), m_ps->GetBufferSize()};
    desc.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
    desc.RasterizerState.CullMode = D3D12_CULL_MODE_BACK;
    desc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
    desc.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
    desc.DepthStencilState.DepthEnable = FALSE;
    desc.DepthStencilState.StencilEnable = FALSE;
    desc.SampleMask = UINT_MAX;
    desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    desc.NumRenderTargets = 1;
    desc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.DSVFormat = DXGI_FORMAT_UNKNOWN;
    desc.SampleDesc.Count = 1;

    // 输入布局：POSITION + NORMAL + TANGENT + TEXCOORD
    D3D12_INPUT_ELEMENT_DESC il[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0,  D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"NORMAL",   0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"TANGENT",  0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 24, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,    0, 36, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0}};
    desc.InputLayout = {il, 4};
    device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&m_pso));

    // ── Unlit PSO（纹理预览，共用根签名，输入布局仅需 POSITION + TEXCOORD） ──
    desc.VS = {m_vsUnlit->GetBufferPointer(), m_vsUnlit->GetBufferSize()};
    desc.PS = {m_psUnlit->GetBufferPointer(), m_psUnlit->GetBufferSize()};
    D3D12_INPUT_ELEMENT_DESC ilUnlit[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0,  D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,    0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0}};
    desc.InputLayout = {ilUnlit, 2};
    device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&m_psoUnlit));
}

// ========================================================================
// 内置球体 mesh（含法线 + 切线，支持 PBR 光照）
// ========================================================================

const Resource::TriangleMesh *PreviewPBRRenderer::CreatePreviewSphere(
    Resource::GpuResourceManager &gpuResMgr,
    Resource::GeometryResourceManager *geoMgr)
{
    if (m_previewSphere) return m_previewSphere.get();

    auto *device = m_context ? m_context->GetDevice() : nullptr;
    auto *cmdMgr = m_context ? &m_context->GetCommandManager() : nullptr;
    if (!device || !cmdMgr) return nullptr;

    // GeometryGenerator 生成球体（自带 Position/Normal/TangentU/TexC）
    GeometryGenerator geoGen;
    auto meshData = geoGen.CreateSphere(1.0f, 32, 16);

    struct SphereVertex { XMFLOAT3 pos; XMFLOAT3 normal; XMFLOAT3 tangent; XMFLOAT2 uv; };
    std::vector<SphereVertex> vertices;
    vertices.reserve(meshData.Vertices.size());
    for (auto &v : meshData.Vertices)
        vertices.push_back({v.Position, v.Normal, v.TangentU, v.TexC});

    std::vector<uint16_t> indices;
    indices.reserve(meshData.Indices32.size());
    for (auto i : meshData.Indices32) indices.push_back((uint16_t)i);

    uint32_t vtxSize = (uint32_t)(vertices.size() * sizeof(SphereVertex));
    uint32_t idxSize = (uint32_t)(indices.size() * sizeof(uint16_t));

    // DEFAULT + UPLOAD 缓冲
    auto vb = gpuResMgr.CreateBuffer(device, vtxSize, L"PreviewSphere_VB", D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COPY_DEST);
    if (!vb.IsValid()) return nullptr;
    auto ib = gpuResMgr.CreateBuffer(device, idxSize, L"PreviewSphere_IB", D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COPY_DEST);
    if (!ib.IsValid()) { gpuResMgr.Release(vb, 0); return nullptr; }

    auto upVB = gpuResMgr.CreateBuffer(device, vtxSize, L"PreviewSphere_UpVB", D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
    if (!upVB.IsValid()) { gpuResMgr.Release(vb, 0); gpuResMgr.Release(ib, 0); return nullptr; }
    auto upIB = gpuResMgr.CreateBuffer(device, idxSize, L"PreviewSphere_UpIB", D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
    if (!upIB.IsValid()) { gpuResMgr.Release(vb, 0); gpuResMgr.Release(ib, 0); gpuResMgr.Release(upVB, 0); return nullptr; }

    {   void *mapped;
        auto *r = gpuResMgr.GetResource(upVB); r->Map(0, nullptr, &mapped); memcpy(mapped, vertices.data(), vtxSize); r->Unmap(0, nullptr);
        r = gpuResMgr.GetResource(upIB); r->Map(0, nullptr, &mapped); memcpy(mapped, indices.data(), idxSize); r->Unmap(0, nullptr);
    }

    // COPY → DIRECT 提交
    uint64_t copyFence = cmdMgr->GetCompletedFenceValue(D3D12_COMMAND_LIST_TYPE_COPY);
    auto cpAllocH = cmdMgr->AcquireAllocator<D3D12_COMMAND_LIST_TYPE_COPY>(copyFence);
    auto *cpAlloc = cmdMgr->GetAllocator<D3D12_COMMAND_LIST_TYPE_COPY>(cpAllocH);
    auto cpCmdH = cmdMgr->AcquireCommandListHandle<D3D12_COMMAND_LIST_TYPE_COPY>(cpAlloc);
    auto cpCmd = cmdMgr->GetCommandList<D3D12_COMMAND_LIST_TYPE_COPY>(cpCmdH);
    cpCmd.Get()->CopyResource(gpuResMgr.GetResource(vb), gpuResMgr.GetResource(upVB));
    cpCmd.Get()->CopyResource(gpuResMgr.GetResource(ib), gpuResMgr.GetResource(upIB));
    cpCmd.Close();

    auto drAllocH = cmdMgr->AcquireAllocator<D3D12_COMMAND_LIST_TYPE_DIRECT>(0);
    auto *drAlloc = cmdMgr->GetAllocator<D3D12_COMMAND_LIST_TYPE_DIRECT>(drAllocH);
    auto drCmdH = cmdMgr->AcquireCommandListHandle<D3D12_COMMAND_LIST_TYPE_DIRECT>(drAlloc);
    auto drCmd = cmdMgr->GetCommandList<D3D12_COMMAND_LIST_TYPE_DIRECT>(drCmdH);
    {
        D3D12_RESOURCE_BARRIER barriers[2] = {
            CD3DX12_RESOURCE_BARRIER::Transition(gpuResMgr.GetResource(vb), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER),
            CD3DX12_RESOURCE_BARRIER::Transition(gpuResMgr.GetResource(ib), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_INDEX_BUFFER)};
        drCmd.Get()->ResourceBarrier(2, barriers);
    }
    drCmd.Close();

    cmdMgr->Submit(D3D12_COMMAND_LIST_TYPE_COPY, cpCmd);
    m_context->FlushCommandQueue(D3D12_COMMAND_LIST_TYPE_COPY);
    cmdMgr->Submit(D3D12_COMMAND_LIST_TYPE_DIRECT, drCmd);
    m_context->FlushCommandQueue(D3D12_COMMAND_LIST_TYPE_DIRECT);

    uint64_t fence = cmdMgr->GetNextSequence();
    gpuResMgr.Release(upVB, fence); gpuResMgr.Release(upIB, fence);
    cmdMgr->ReleaseAllocator<D3D12_COMMAND_LIST_TYPE_COPY>(cpAllocH, fence);
    cmdMgr->ReleaseAllocator<D3D12_COMMAND_LIST_TYPE_DIRECT>(drAllocH, fence);

    auto mesh = std::make_unique<Resource::TriangleMesh>();
    mesh->vertexBufferHandle = vb;
    mesh->indexBufferHandle = ib;
    mesh->vertexCount = (uint32_t)vertices.size();
    mesh->indexCount = (uint32_t)indices.size();
    mesh->vertexStride = sizeof(SphereVertex);
    mesh->indexFormat = DXGI_FORMAT_R16_UINT;
    mesh->topology = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
    mesh->isGpuReady = true;
    m_previewSphere = std::move(mesh);
    return m_previewSphere.get();
}

