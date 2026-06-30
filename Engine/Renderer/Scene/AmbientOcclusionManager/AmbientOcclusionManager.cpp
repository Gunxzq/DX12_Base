#include "AmbientOcclusionManager.h"
#include "Renderer/RHI/D3D12DeviceContext.h"
#include "Resource/Core/DescriptorHeapCollection.h"
#include <d3dx12.h>
#include <random>
#include <vector>

using namespace DX12Engine::Resource;

namespace DX12Engine::Renderer {

static constexpr DXGI_FORMAT kNormalMapFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;
static constexpr DXGI_FORMAT kAmbientMapFormat = DXGI_FORMAT_R16_UNORM;

// ========================================================================
// 单例
// ========================================================================

AmbientOcclusionManager &AmbientOcclusionManager::GetInstance() {
    static AmbientOcclusionManager s_instance;
    return s_instance;
}

// ========================================================================
// 生命周期
// ========================================================================

void AmbientOcclusionManager::SetDeviceContext(D3D12DeviceContext *context) {
    m_deviceContext = context;
    m_ssaoRenderer.SetDeviceContext(context);
}

void AmbientOcclusionManager::Initialize(ID3D12Device *device, Resource::DescriptorHeapCollection *descriptorHeaps,
                                         uint32_t renderWidth, uint32_t renderHeight) {
    if (m_initialized) {
        Shutdown();
    }

    m_device = device;
    m_descriptorHeaps = descriptorHeaps;

    if (!m_device || !m_descriptorHeaps) {
        return;
    }

    for (auto &pso : m_algorithmPSOs) {
        pso = nullptr;
    }

    m_currentAlgorithm = AoAlgorithm::SSAO;

#ifdef WITH_EDITOR
    m_bakingEnabled = true;
#else
    m_bakingEnabled = false;
#endif

    // 分配 RTV 池资源
    BuildResources(renderWidth, renderHeight);

    // 初始化 SSAO 渲染器
    m_ssaoRenderer.SetDescriptorHeaps(m_descriptorHeaps);
    m_ssaoRenderer.Initialize();

    // 注册 SSAO PSO（由 SsaoRenderer 内部创建）

    // 注册 SSAO PSO（由 SsaoRenderer 内部创建）
    if (m_ssaoRenderer.GetSSAOPipeline()) {
        SetAlgorithmPSO(AoAlgorithm::SSAO, m_ssaoRenderer.GetSSAOPipeline());
    }

    m_initialized = true;
}

void AmbientOcclusionManager::Shutdown() {
    if (!m_initialized)
        return;

    m_ssaoRenderer.Shutdown();
    m_randomVectorTexture.Reset();
    ReleaseResources();

    for (auto &pso : m_algorithmPSOs) {
        pso = nullptr;
    }

    m_normalSRV = {};
    m_ambientSRV = {};
    m_ambientRTV = {};
    m_ambient1SRV = {};
    m_ambient1RTV = {};
    m_privateDepthDSV = {};
    m_privateDepthSRV = {};
    m_bakingEnabled = false;
    m_device = nullptr;
    m_descriptorHeaps = nullptr;
    m_initialized = false;
}

// ========================================================================
// 资源管理
// ========================================================================

void AmbientOcclusionManager::BuildResources(uint32_t width, uint32_t height) {
    if (width == 0 || height == 0)
        return;

    auto &rtvPool = RenderTargetPool::GetInstance();

    // 法线贴图
    {
        RenderTargetDesc desc;
        desc.width = width;
        desc.height = height;
        desc.format = kNormalMapFormat;
        desc.flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        desc.clearValue = {kNormalMapFormat, {0.0f, 0.0f, 1.0f, 0.0f}};
        m_normalRT = rtvPool.Allocate(desc);
        if (m_normalRT.IsValid()) {
            m_normalSRV = rtvPool.GetSrvHandle(m_normalRT);
            m_normalRTV = rtvPool.GetRtvHandle(m_normalRT);
        }
    }

    // AO Map 0（用于 ComputeAO 输出 + 最终结果）
    {
        RenderTargetDesc desc;
        desc.width = width;
        desc.height = height;
        desc.format = kAmbientMapFormat;
        desc.flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        desc.clearValue = {kAmbientMapFormat, {1.0f, 0.0f, 0.0f, 0.0f}};
        m_ambientRT0 = rtvPool.Allocate(desc);
        if (m_ambientRT0.IsValid()) {
            m_ambientSRV = rtvPool.GetSrvHandle(m_ambientRT0);
            m_ambientRTV = rtvPool.GetRtvHandle(m_ambientRT0);
        }
    }

    // AO Map 1（模糊 ping-pong）
    {
        RenderTargetDesc desc;
        desc.width = width;
        desc.height = height;
        desc.format = kAmbientMapFormat;
        desc.flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        desc.clearValue = {kAmbientMapFormat, {1.0f, 0.0f, 0.0f, 0.0f}};
        m_ambientRT1 = rtvPool.Allocate(desc);
        if (m_ambientRT1.IsValid()) {
            m_ambient1SRV = rtvPool.GetSrvHandle(m_ambientRT1);
            m_ambient1RTV = rtvPool.GetRtvHandle(m_ambientRT1);
        }
    }

    // 私有深度缓冲（DrawNormals 写入，不污染主 DSV）
    {
        auto &dsPool = DepthStencilPool::GetInstance();
        DXGI_FORMAT depthFormat = DXGI_FORMAT_D32_FLOAT;
        DepthStencilDesc dsDesc;
        dsDesc.width = width;
        dsDesc.height = height;
        dsDesc.format = depthFormat;
        dsDesc.flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
        dsDesc.clearValue = {depthFormat, {1.0f, 0}};
        m_privateDepth = dsPool.Allocate(dsDesc);
        if (m_privateDepth.IsValid()) {
            m_privateDepthDSV = dsPool.GetDsvHandle(m_privateDepth);
            m_privateDepthSRV = dsPool.GetSrvHandle(m_privateDepth);

            WCHAR buf[256];
            swprintf_s(buf, L"[SSAO] PrivateDepth: poolSize=%u allocCount=%u dsvSRV.ptr=0x%llx srvSRV.ptr=0x%llx\n",
                       dsPool.GetPoolSize(), dsPool.GetAllocatedCount(),
                       m_privateDepthDSV.ptr, m_privateDepthSRV.ptr);
            OutputDebugStringW(buf);
        } else {
            OutputDebugStringW(L"[SSAO] WARNING: PrivateDepth allocation FAILED!\n");
        }
    }
}

void AmbientOcclusionManager::ReleaseResources() {
    auto &rtvPool = RenderTargetPool::GetInstance();
    if (m_normalRT.IsValid()) {
        rtvPool.Free(m_normalRT, UINT64_MAX);
        m_normalRT = {};
    }
    if (m_ambientRT0.IsValid()) {
        rtvPool.Free(m_ambientRT0, UINT64_MAX);
        m_ambientRT0 = {};
    }
    if (m_ambientRT1.IsValid()) {
        rtvPool.Free(m_ambientRT1, UINT64_MAX);
        m_ambientRT1 = {};
    }
    if (m_privateDepth.IsValid()) {
        auto &dsPool = DepthStencilPool::GetInstance();
        dsPool.Free(m_privateDepth, UINT64_MAX);
        m_privateDepth = {};
    }
}

// ========================================================================
// 随机向量纹理（4×4 噪音，供 SSAO 采样旋转使用）
// ========================================================================

void AmbientOcclusionManager::BuildRandomVectorTexture() {
    auto &cmdMgr = m_deviceContext->GetCommandManager();
    auto *device = m_device;
    if (!device)
        return;

    // 龙书标准：256x256 随机向量纹理
    constexpr UINT kRandomTexSize = 256;

    // 生成随机数据
    struct RandomVec { uint8_t r, g, b, a; };
    std::vector<RandomVec> s_data(kRandomTexSize * kRandomTexSize);
    std::mt19937 rng(42);
    std::uniform_real_distribution<float> dist(0.0f, 1.0f);
    for (auto &v : s_data) {
        v.r = static_cast<uint8_t>(dist(rng) * 255.0f);
        v.g = static_cast<uint8_t>(dist(rng) * 255.0f);
        v.b = static_cast<uint8_t>(dist(rng) * 255.0f);
        v.a = 255;
    }

    D3D12_RESOURCE_DESC texDesc = {};
    texDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    texDesc.Width = kRandomTexSize;
    texDesc.Height = kRandomTexSize;
    texDesc.DepthOrArraySize = 1;
    texDesc.MipLevels = 1;
    texDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    texDesc.SampleDesc.Count = 1;
    texDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    texDesc.Flags = D3D12_RESOURCE_FLAG_NONE;

    CD3DX12_HEAP_PROPERTIES heapProps(D3D12_HEAP_TYPE_DEFAULT);
    HRESULT hr =
        device->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &texDesc, D3D12_RESOURCE_STATE_COPY_DEST,
                                        nullptr, IID_PPV_ARGS(&m_randomVectorTexture));
    if (FAILED(hr))
        return;

    // 上传
    D3D12_SUBRESOURCE_DATA subData = {};
    subData.pData = s_data.data();
    subData.RowPitch = kRandomTexSize * 4;
    subData.SlicePitch = subData.RowPitch * kRandomTexSize;

    UINT64 uploadSize = GetRequiredIntermediateSize(m_randomVectorTexture.Get(), 0, 1);
    Microsoft::WRL::ComPtr<ID3D12Resource> uploadBuf;
    CD3DX12_HEAP_PROPERTIES uploadHeapProps(D3D12_HEAP_TYPE_UPLOAD);
    auto uploadDesc = CD3DX12_RESOURCE_DESC::Buffer(uploadSize);
    device->CreateCommittedResource(&uploadHeapProps, D3D12_HEAP_FLAG_NONE, &uploadDesc,
                                    D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&uploadBuf));

    // 使用命令管理器标准路径上传
    auto allocHandle = cmdMgr.AcquireAllocator<D3D12_COMMAND_LIST_TYPE_DIRECT>(0);
    auto *allocator = cmdMgr.GetAllocator<D3D12_COMMAND_LIST_TYPE_DIRECT>(allocHandle);
    auto cmdHandle = cmdMgr.AcquireCommandListHandle<D3D12_COMMAND_LIST_TYPE_DIRECT>(allocator);
    auto cmdList = cmdMgr.GetCommandList<D3D12_COMMAND_LIST_TYPE_DIRECT>(cmdHandle);

    UpdateSubresources(cmdList.Get(), m_randomVectorTexture.Get(), uploadBuf.Get(), 0, 0, 1, &subData);

    auto barrier = CD3DX12_RESOURCE_BARRIER::Transition(m_randomVectorTexture.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
                                                        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    cmdList.Get()->ResourceBarrier(1, &barrier);
    cmdList.Close();

    cmdMgr.Submit(D3D12_COMMAND_LIST_TYPE_DIRECT, cmdList);
    cmdMgr.Flush(D3D12_COMMAND_LIST_TYPE_DIRECT);

    uint64_t seq = cmdMgr.GetNextSequence();
    cmdMgr.ReleaseCommandList<D3D12_COMMAND_LIST_TYPE_DIRECT>(cmdHandle);
    cmdMgr.ReleaseAllocator<D3D12_COMMAND_LIST_TYPE_DIRECT>(allocHandle, seq);

    // 创建 SRV 并注入 SsaoRenderer
    if (m_descriptorHeaps) {
        uint32_t slot = m_descriptorHeaps->Allocate(PartitionType::Texture);
        char buf[128];
        if (slot == UINT32_MAX) {
            sprintf_s(buf, "[SSAO] BuildRandomVectorTexture: Allocate SRV slot FAILED (Texture partition may be full)\n");
            OutputDebugStringA(buf);
        } else {
            sprintf_s(buf, "[SSAO] BuildRandomVectorTexture: SRV slot=%u\n", slot);
            OutputDebugStringA(buf);
            D3D12_CPU_DESCRIPTOR_HANDLE cpuHandle =
                m_descriptorHeaps->GetPartitionCpuHandle(PartitionType::Texture, slot);
            D3D12_GPU_DESCRIPTOR_HANDLE gpuHandle =
                m_descriptorHeaps->GetPartitionGpuHandle(PartitionType::Texture, slot);

            D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
            srvDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            srvDesc.Texture2D.MipLevels = 1;
            device->CreateShaderResourceView(m_randomVectorTexture.Get(), &srvDesc, cpuHandle);

            m_ssaoRenderer.SetRandomVectorSRV(gpuHandle);
            sprintf_s(buf, "[SSAO] RandomVec SRV created: ptr=0x%llx\n", gpuHandle.ptr);
            OutputDebugStringA(buf);
        }
    }
}

// ========================================================================
// AO RT 初始状态过渡（在命令管理器就绪后调用，与 BuildRandomVectorTexture 同一阶段）
// ========================================================================

void AmbientOcclusionManager::InitializeResourceStates() {
    if (!m_deviceContext)
        return;
    auto *res0 = GetAmbientResource0();
    auto *res1 = GetAmbientResource1();
    auto *resN = GetNormalResource();
    if (!res0 && !res1 && !resN)
        return;

    auto &cmdMgr = m_deviceContext->GetCommandManager();
    auto allocH = cmdMgr.AcquireAllocator<D3D12_COMMAND_LIST_TYPE_DIRECT>(0);
    auto *alloc = cmdMgr.GetAllocator<D3D12_COMMAND_LIST_TYPE_DIRECT>(allocH);
    auto cmdH = cmdMgr.AcquireCommandListHandle<D3D12_COMMAND_LIST_TYPE_DIRECT>(alloc);
    auto cmdL = cmdMgr.GetCommandList<D3D12_COMMAND_LIST_TYPE_DIRECT>(cmdH);
    auto transition = [&](ID3D12Resource *res) {
        if (!res)
            return;
        auto b = CD3DX12_RESOURCE_BARRIER::Transition(res, D3D12_RESOURCE_STATE_COMMON,
                                                      D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        cmdL.Get()->ResourceBarrier(1, &b);
    };
    transition(resN);
    transition(res0);
    transition(res1);

    cmdL.Close();
    cmdMgr.Submit(D3D12_COMMAND_LIST_TYPE_DIRECT, cmdL);
    cmdMgr.Flush(D3D12_COMMAND_LIST_TYPE_DIRECT);
    auto seq = cmdMgr.GetNextSequence();
    cmdMgr.ReleaseCommandList<D3D12_COMMAND_LIST_TYPE_DIRECT>(cmdH);
    cmdMgr.ReleaseAllocator<D3D12_COMMAND_LIST_TYPE_DIRECT>(allocH, seq);
}

// ========================================================================
// CPU SRV → GPU SRV（供 SsaoRenderer 绑定使用）
// ========================================================================

D3D12_GPU_DESCRIPTOR_HANDLE AmbientOcclusionManager::CpuSrvToGpu(D3D12_CPU_DESCRIPTOR_HANDLE cpuHandle) const {
    if (cpuHandle.ptr == 0 || !m_descriptorHeaps)
        return {};

    ID3D12DescriptorHeap *heap = m_descriptorHeaps->GetHeap(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    if (!heap)
        return {};

    D3D12_CPU_DESCRIPTOR_HANDLE cpuBase = heap->GetCPUDescriptorHandleForHeapStart();
    D3D12_GPU_DESCRIPTOR_HANDLE gpuBase = heap->GetGPUDescriptorHandleForHeapStart();
    UINT descSize = m_descriptorHeaps->GetDescriptorSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    UINT offset = static_cast<UINT>((cpuHandle.ptr - cpuBase.ptr) / descSize);
    D3D12_GPU_DESCRIPTOR_HANDLE gpuHandle;
    gpuHandle.ptr = gpuBase.ptr + static_cast<UINT64>(offset) * descSize;
    return gpuHandle;
}

// ========================================================================
// 算法切换
// ========================================================================

void AmbientOcclusionManager::SetAlgorithm(AoAlgorithm algo) {
    if (algo < AoAlgorithm::Count) {
        m_currentAlgorithm = algo;
    }
}

const char *AmbientOcclusionManager::GetAlgorithmName() const {
    switch (m_currentAlgorithm) {
    case AoAlgorithm::SSAO:
        return "SSAO";
    case AoAlgorithm::HBAO:
        return "HBAO";
    default:
        return "Unknown";
    }
}

ID3D12PipelineState *AmbientOcclusionManager::GetCurrentPSO() const {
    return m_algorithmPSOs[static_cast<uint32_t>(m_currentAlgorithm)];
}

void AmbientOcclusionManager::SetAlgorithmPSO(AoAlgorithm algo, ID3D12PipelineState *pso) {
    if (algo < AoAlgorithm::Count) {
        m_algorithmPSOs[static_cast<uint32_t>(algo)] = pso;
    }
}

// ---- 资源屏障辅助 ----

ID3D12Resource *AmbientOcclusionManager::GetNormalResource() const {
    return m_normalRT.IsValid() ? RenderTargetPool::GetInstance().GetResource(m_normalRT) : nullptr;
}

ID3D12Resource *AmbientOcclusionManager::GetAmbientResource0() const {
    return m_ambientRT0.IsValid() ? RenderTargetPool::GetInstance().GetResource(m_ambientRT0) : nullptr;
}

ID3D12Resource *AmbientOcclusionManager::GetAmbientResource1() const {
    return m_ambientRT1.IsValid() ? RenderTargetPool::GetInstance().GetResource(m_ambientRT1) : nullptr;
}

ID3D12Resource *AmbientOcclusionManager::GetPrivateDepthResource() const {
    return m_privateDepth.IsValid() ? DepthStencilPool::GetInstance().GetResource(m_privateDepth) : nullptr;
}

// ========================================================================
// AO 计算入口
// ========================================================================

void AmbientOcclusionManager::Execute(ID3D12GraphicsCommandList *cmdList, D3D12_GPU_DESCRIPTOR_HANDLE depthSRV,
                                      const DirectX::XMFLOAT4X4 &viewProj) {
    if (!m_initialized || !cmdList)
        return;

    // 包装为 CommandList（与 SsaoRenderer 接口匹配）
    CommandList wrappedCmdList(cmdList);

    // 获取当前的 AO PSO 和模糊 PSO
    ID3D12PipelineState *aoPSO = GetCurrentPSO();
    ID3D12PipelineState *blurPSO = m_ssaoRenderer.GetBlurPipeline();

    // 执行 SSAO 管线
    m_ssaoRenderer.Execute(wrappedCmdList, aoPSO, blurPSO, depthSRV,
                           GetNormalMapSRV(),   // normalSRV
                           GetAmbientMapSRV(),  // ambientSRV
                           GetAmbientMapRTV(),  // ambientRTV
                           GetAmbientMap1SRV(), // ambient1SRV
                           GetAmbientMap1RTV(), // ambient1RTV
                           GetAmbientResource0(), // ambientRes0
                           GetAmbientResource1(), // ambientRes1
                           viewProj);           // viewProj
}

// ========================================================================
// 静态烘培
// ========================================================================

void AmbientOcclusionManager::BakeStaticAO(uint32_t regionX, uint32_t regionY, uint32_t regionWidth,
                                           uint32_t regionHeight) {
    if (!m_bakingEnabled)
        return;
    (void)regionX;
    (void)regionY;
    (void)regionWidth;
    (void)regionHeight;
}

} // namespace DX12Engine::Renderer
