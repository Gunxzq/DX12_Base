#include "WireframeManager.h"
#include "Common/d3dUtil.h"
#include "Logger/Logger.h"
#include "Math/BoundingVolume.h"
#include "Resource/GpuResourceManager.h"
#include <cstring>

namespace DX12Engine::Renderer {

// ========================================================================
// 单例
// ========================================================================

namespace {
// 节流日志：每 N 帧打印一次，避免每帧刷屏
bool ThrottledLog(uint32_t &frameCounter, uint32_t interval) {
    frameCounter = (frameCounter + 1) % interval;
    return frameCounter == 0;
}
} // namespace

WireframeManager &WireframeManager::GetInstance() {
    static WireframeManager instance;
    return instance;
}

// ========================================================================
// 初始化 / 清理
// ========================================================================

void WireframeManager::Initialize(ID3D12Device *device, DXGI_FORMAT depthFormat) {
    if (m_initialized)
        return;
    if (!device)
        return;
    m_device = device;

    // ── 动态线列表 VB（UPLOAD 堆，固定容量：64KB ≈ 2285 线段，调试线框量级足够） ──
    m_vbCapacityBytes = 64 * 1024;
    m_lineVB = Resource::GpuResourceManager::GetInstance().CreateBuffer(
        device, m_vbCapacityBytes, L"Wireframe_LineVB", D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
    if (!m_lineVB.IsValid())
        return;

    // ── 参数 CB（viewProj + 屏幕尺寸 + 线宽） ──
    m_lineCB = Resource::GpuResourceManager::GetInstance().CreateBuffer(
        device, 256, L"Wireframe_LineCB", D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
    if (!m_lineCB.IsValid())
        return;

    ID3D12Resource *cbRes = Resource::GpuResourceManager::GetInstance().GetResource(m_lineCB);
    if (cbRes) {
        m_lineCBAddress = cbRes->GetGPUVirtualAddress();
    }

    LoadShaders(device);
    CreateRootSignature();
    // 规则 16：深度格式取自主交换链（Editor/Game 共用配置），禁止硬编码
    CreatePSO(device, depthFormat);

    m_initialized = true;
}

void WireframeManager::Shutdown() {
    if (!m_initialized)
        return;

    m_pso.Reset();
    m_rootSig.Reset();
    m_vsBlob.Reset();
    m_gsBlob.Reset();
    m_psBlob.Reset();

    auto &gpuMgr = Resource::GpuResourceManager::GetInstance();
    if (m_lineCB.IsValid()) {
        gpuMgr.Release(m_lineCB, 0);
        m_lineCB = Resource::GpuResourceHandle::Invalid();
    }
    if (m_lineVB.IsValid()) {
        gpuMgr.Release(m_lineVB, 0);
        m_lineVB = Resource::GpuResourceHandle::Invalid();
    }

    m_lineCBAddress = 0;
    m_vbCapacityBytes = 0;
    m_lines.clear();
    m_device = nullptr;
    m_initialized = false;
}

// ========================================================================
// 声明式收集（CPU 侧，无 GPU、顺序无关）
// ========================================================================

void WireframeManager::AddLine(const DirectX::XMFLOAT3 &a, const DirectX::XMFLOAT3 &b, DirectX::XMFLOAT4 color) {
    if (!m_initialized)
        return;
    // 容量保护：2 顶点/线段，超出 VB 容量则丢弃（调试线框量级远小于容量）
    if (m_lines.size() + 2 > m_vbCapacityBytes / sizeof(LineVertex))
        return;
    m_lines.push_back({a, color});
    m_lines.push_back({b, color});
}

void WireframeManager::AddAABB(const DX12Engine::Math::BoundingAABB &aabb, const DirectX::XMMATRIX &world,
                               DirectX::XMFLOAT4 color) {
    using namespace DirectX;
    if (!m_initialized)
        return;

    // 8 个局部空间角点（布局：0-3 面 A(z=min)，4-7 面 B(z=max)，i↔i+4 对应）
    XMFLOAT3 localCorners[8] = {
        {aabb.min.x, aabb.min.y, aabb.min.z}, {aabb.max.x, aabb.min.y, aabb.min.z},
        {aabb.min.x, aabb.max.y, aabb.min.z}, {aabb.max.x, aabb.max.y, aabb.min.z},
        {aabb.min.x, aabb.min.y, aabb.max.z}, {aabb.max.x, aabb.min.y, aabb.max.z},
        {aabb.min.x, aabb.max.y, aabb.max.z}, {aabb.max.x, aabb.max.y, aabb.max.z},
    };

    XMFLOAT3 worldCorners[8];
    for (int i = 0; i < 8; ++i) {
        XMStoreFloat3(&worldCorners[i], XMVector3Transform(XMLoadFloat3(&localCorners[i]), world));
    }

    // 12 条边：面 A 4 + 面 B 4 + 连接 4
    static const int kEdges[12][2] = {
        {0, 1}, {1, 3}, {3, 2}, {2, 0}, // 面 A
        {4, 5}, {5, 7}, {7, 6}, {6, 4}, // 面 B
        {0, 4}, {1, 5}, {2, 6}, {3, 7}, // 连接
    };
    for (int i = 0; i < 12; ++i) {
        AddLine(worldCorners[kEdges[i][0]], worldCorners[kEdges[i][1]], color);
    }
}

void WireframeManager::AddFrustum(const Frustum &frustum, const DirectX::XMFLOAT3 &cameraPosition,
                                  DirectX::XMFLOAT4 nearColor, DirectX::XMFLOAT4 farColor,
                                  DirectX::XMFLOAT4 connectColor) {
    if (!m_initialized)
        return;

    // Frustum::GetCorners() 布局：0-3 近平面(BL/BR/TL/TR)，4-7 远平面(BL/BR/TL/TR)，i↔i+4 对应
    const auto &corners = frustum.GetCorners();

    // ── 近裁剪面矩形 4 条边（BL-BR 底、BR-TR 右、TR-TL 顶、TL-BL 左） ──
    AddLine(corners[0], corners[1], nearColor);
    AddLine(corners[1], corners[3], nearColor);
    AddLine(corners[3], corners[2], nearColor);
    AddLine(corners[2], corners[0], nearColor);

    // ── 远裁剪面矩形 4 条边 ──
    AddLine(corners[4], corners[5], farColor);
    AddLine(corners[5], corners[7], farColor);
    AddLine(corners[7], corners[6], farColor);
    AddLine(corners[6], corners[4], farColor);

    // ── 汇聚线：从相机位置 → 远平面 4 角点（Blender 风格） ──
    // 参考 Blender：视锥体线必须汇聚到相机位置（否则近/远两个矩形各自独立，
    // 视觉上像两个大小不一的锥体，找不到相机在哪）。远平面角点 4-7 各引一条线到相机点。
    AddLine(cameraPosition, corners[4], connectColor);
    AddLine(cameraPosition, corners[5], connectColor);
    AddLine(cameraPosition, corners[6], connectColor);
    AddLine(cameraPosition, corners[7], connectColor);
}

void WireframeManager::Clear() { m_lines.clear(); }

// ========================================================================
// 固定录制点
// ========================================================================

void WireframeManager::UpdateAndUpload(uint64_t fence, const Camera &camera) {
    (void)fence;
    if (!m_initialized || !m_lineVB.IsValid() || !m_lineCBAddress)
        return;

    // ── 写 CB（viewProj + 屏幕参数 + 距离自适应线宽参考距离） ──
    ID3D12Resource *cbRes = Resource::GpuResourceManager::GetInstance().GetResource(m_lineCB);
    if (cbRes) {
        LineCBData cbData;
        cbData.viewProj = camera.ViewProjMatrix;
        cbData.screenWidth = m_screenWidth;
        cbData.screenHeight = m_screenHeight;
        cbData.lineWidth = m_lineWidth;
        cbData.referenceDistance = m_referenceDistance;

        void *mapped = nullptr;
        HRESULT mapHr = cbRes->Map(0, nullptr, &mapped);
        // 2026-08-19 防御加固：Map 失败（HRESULT 失败 / mapped 为 nullptr）时资源【从未被映射】——
        // 绝不 Unmap（#310）也不 memcpy（写 0x0 崩溃），跳过上传并记录
        if (FAILED(mapHr) || !mapped) {
            Logger::Logger::GetInstance()->Warn("[WireframeManager] LineCB Map failed: hr=0x{:08X}", (unsigned)mapHr);
            return;
        }
        memcpy(mapped, &cbData, sizeof(cbData));
        cbRes->Unmap(0, nullptr);
    }

    // ── 写线列表到动态 VB ──
    ID3D12Resource *vbRes = Resource::GpuResourceManager::GetInstance().GetResource(m_lineVB);
    if (!vbRes)
        return;

    size_t byteSize = m_lines.size() * sizeof(LineVertex);
    if (byteSize > m_vbCapacityBytes)
        byteSize = m_vbCapacityBytes; // 截断保护（理论上不会触发）

    void *mapped = nullptr;
    HRESULT mapHr = vbRes->Map(0, nullptr, &mapped);
    // 2026-08-19 防御加固：Map 失败时绝不 Unmap（#310）/memcpy（写 0x0），跳过并记录
    if (FAILED(mapHr) || !mapped) {
        Logger::Logger::GetInstance()->Warn("[WireframeManager] LineVB Map failed: hr=0x{:08X}", (unsigned)mapHr);
        return;
    }
    if (byteSize > 0) {
        memcpy(mapped, m_lines.data(), byteSize);
    }
    vbRes->Unmap(0, nullptr);

    // 记录上传线数并清空收集列表（Draw 依据 m_uploadedLineCount，与 m_lines 解耦，
    // 避免 m_lines 无限累积——此前 Clear() 从未被调用导致每帧线数叠加）
    m_uploadedLineCount = m_lines.size();
    m_lines.clear();

    // ── 节流日志：确认上传路径与线数 ──
    static uint32_t s_logFrame = 0;
    if (ThrottledLog(s_logFrame, 120)) {
        Logger::Logger::GetInstance()->Info("[WireframeManager] UpdateAndUpload: {} lines, screen={}x{}",
                                            m_uploadedLineCount, m_screenWidth, m_screenHeight);
    }
}

void WireframeManager::Draw(ID3D12GraphicsCommandList *cmdList) {
    if (!m_initialized || !m_pso || !m_rootSig || !m_visible)
        return;
    if (m_uploadedLineCount == 0)
        return;

    ID3D12Resource *vbRes = Resource::GpuResourceManager::GetInstance().GetResource(m_lineVB);
    if (!vbRes)
        return;

    // ── 节流日志：确认 Draw 是否被录制 ──
    static uint32_t s_logFrame = 0;
    if (ThrottledLog(s_logFrame, 120)) {
        Logger::Logger::GetInstance()->Info("[WireframeManager] Draw: recording {} lines (LINELIST, PSO={})",
                                            m_uploadedLineCount, m_pso ? "valid" : "null");
    }

    cmdList->SetPipelineState(m_pso.Get());
    cmdList->SetGraphicsRootSignature(m_rootSig.Get());
    cmdList->SetGraphicsRootConstantBufferView(0, m_lineCBAddress);

    D3D12_VERTEX_BUFFER_VIEW vbView = {};
    vbView.BufferLocation = vbRes->GetGPUVirtualAddress();
    vbView.SizeInBytes = static_cast<UINT>(m_uploadedLineCount * sizeof(LineVertex));
    vbView.StrideInBytes = sizeof(LineVertex);
    cmdList->IASetVertexBuffers(0, 1, &vbView);

    cmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_LINELIST);
    cmdList->DrawInstanced(static_cast<UINT>(m_uploadedLineCount), 1, 0, 0);
}

// ========================================================================
// 内部初始化
// ========================================================================

void WireframeManager::LoadShaders(ID3D12Device *device) {
    (void)device;
    m_vsBlob = d3dUtil::CompileShader(L"Shaders/line.hlsl", nullptr, "VS", "vs_5_1");
    m_gsBlob = d3dUtil::CompileShader(L"Shaders/line.hlsl", nullptr, "GS", "gs_5_1");
    m_psBlob = d3dUtil::CompileShader(L"Shaders/line.hlsl", nullptr, "PS", "ps_5_1");
}

void WireframeManager::CreateRootSignature() {
    if (!m_device)
        return;

    // 根参数：b0 = LineCB (viewProj + 屏幕参数 + 线宽)
    CD3DX12_ROOT_PARAMETER rootParam = {};
    rootParam.ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    rootParam.Descriptor.ShaderRegister = 0;
    rootParam.Descriptor.RegisterSpace = 0;
    rootParam.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    CD3DX12_ROOT_SIGNATURE_DESC rootSigDesc = {};
    rootSigDesc.Init(1, &rootParam, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT);

    Microsoft::WRL::ComPtr<ID3DBlob> sigBlob;
    HRESULT hr = D3D12SerializeRootSignature(&rootSigDesc, D3D_ROOT_SIGNATURE_VERSION_1, &sigBlob, nullptr);
    if (FAILED(hr))
        return;
    hr = m_device->CreateRootSignature(0, sigBlob->GetBufferPointer(), sigBlob->GetBufferSize(),
                                       IID_PPV_ARGS(&m_rootSig));
}

void WireframeManager::CreatePSO(ID3D12Device *device, DXGI_FORMAT depthFormat) {
    if (!m_device || !m_rootSig || !m_vsBlob || !m_gsBlob || !m_psBlob)
        return;

    D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc = {};
    psoDesc.pRootSignature = m_rootSig.Get();
    psoDesc.VS = {m_vsBlob->GetBufferPointer(), m_vsBlob->GetBufferSize()};
    psoDesc.GS = {m_gsBlob->GetBufferPointer(), m_gsBlob->GetBufferSize()};
    psoDesc.PS = {m_psBlob->GetBufferPointer(), m_psBlob->GetBufferSize()};

    // 光栅化：GS 展开的三角形绕序不固定，CullMode 必须 NONE
    psoDesc.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
    psoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;

    // 混合：SRC_ALPHA 混合（半透明调试线），alpha 通道保留目标值
    D3D12_BLEND_DESC blendDesc = {};
    blendDesc.RenderTarget[0].BlendEnable = TRUE;
    blendDesc.RenderTarget[0].SrcBlend = D3D12_BLEND_SRC_ALPHA;
    blendDesc.RenderTarget[0].DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
    blendDesc.RenderTarget[0].BlendOp = D3D12_BLEND_OP_ADD;
    blendDesc.RenderTarget[0].SrcBlendAlpha = D3D12_BLEND_ZERO;
    blendDesc.RenderTarget[0].DestBlendAlpha = D3D12_BLEND_ONE;
    blendDesc.RenderTarget[0].BlendOpAlpha = D3D12_BLEND_OP_ADD;
    blendDesc.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    psoDesc.BlendState = blendDesc;

    // 深度：调试线框是叠加层，须始终可见——深度测试 ALWAYS 通过（不写深度，不污染深度缓冲）
    // 原 LESS_EQUAL 会用场景深度缓冲剔除线框（场景物体后方的线段不可见），改为 ALWAYS 修复
    psoDesc.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
    psoDesc.DepthStencilState.DepthEnable = TRUE;
    psoDesc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
    psoDesc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;

    psoDesc.SampleMask = UINT_MAX;
    psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE;
    psoDesc.NumRenderTargets = 1;
    psoDesc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    psoDesc.DSVFormat = depthFormat;
    psoDesc.SampleDesc.Count = 1;

    D3D12_INPUT_ELEMENT_DESC inputLayout[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
    };
    psoDesc.InputLayout.NumElements = 2;
    psoDesc.InputLayout.pInputElementDescs = inputLayout;

    device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&m_pso));
}

} // namespace DX12Engine::Renderer
