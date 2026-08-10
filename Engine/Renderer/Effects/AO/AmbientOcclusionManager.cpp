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
                                         uint32_t renderWidth, uint32_t renderHeight, Resource::HeapTag heapTag) {
    if (m_initialized) {
        Shutdown();
    }

    m_device = device;
    m_descriptorHeaps = descriptorHeaps;
    m_heapTag = heapTag;

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
    m_ssaoRenderer.SetHeapTag(m_heapTag); // 规则 17：SSAO pass 绑定与 AO RT 同域的描述符堆
    m_ssaoRenderer.Initialize();

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

    m_ambientSRV = {};
    m_ambientRTV = {};
    m_ambient1SRV = {};
    m_ambient1RTV = {};
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

    // AO Map 0（用于 ComputeAO 输出 + 最终结果）
    {
        RenderTargetDesc desc;
        desc.width = width;
        desc.height = height;
        desc.format = kAmbientMapFormat;
        desc.flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        desc.clearValue = {kAmbientMapFormat, {1.0f, 0.0f, 0.0f, 0.0f}};
        desc.name = L"SSAO_Ambient0";
        m_ambientRT0 = rtvPool.Allocate(desc, m_heapTag);
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
        desc.name = L"SSAO_Ambient1";
        m_ambientRT1 = rtvPool.Allocate(desc, m_heapTag);
        if (m_ambientRT1.IsValid()) {
            m_ambient1SRV = rtvPool.GetSrvHandle(m_ambientRT1);
            m_ambient1RTV = rtvPool.GetRtvHandle(m_ambientRT1);
        }
    }

    m_renderWidth = width;
    m_renderHeight = height;
    // 屏障用
}

void AmbientOcclusionManager::ReleaseResources() {
    auto &rtvPool = RenderTargetPool::GetInstance();
    if (m_ambientRT0.IsValid()) {
        rtvPool.Free(m_ambientRT0, UINT64_MAX);
        m_ambientRT0 = {};
    }
    if (m_ambientRT1.IsValid()) {
        rtvPool.Free(m_ambientRT1, UINT64_MAX);
        m_ambientRT1 = {};
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
    struct RandomVec {
        uint8_t r, g, b, a;
    };
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
    m_randomVectorTexture->SetName(L"SSAO_RandomVector");

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
    if (uploadBuf)
        uploadBuf->SetName(L"SSAO_RandomVector_Upload");

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
        uint32_t slot = m_descriptorHeaps->Allocate(m_heapTag, PartitionType::Texture);
        char buf[128];
        if (slot == UINT32_MAX) {
            sprintf_s(buf,
                      "[SSAO] BuildRandomVectorTexture: Allocate SRV slot FAILED (Texture partition may be full)\n");
            OutputDebugStringA(buf);
        } else {
            sprintf_s(buf, "[SSAO] BuildRandomVectorTexture: SRV slot=%u\n", slot);
            OutputDebugStringA(buf);
            D3D12_CPU_DESCRIPTOR_HANDLE cpuHandle =
                m_descriptorHeaps->GetPartitionCpuHandle(PartitionType::Texture, slot, m_heapTag);
            D3D12_GPU_DESCRIPTOR_HANDLE gpuHandle =
                m_descriptorHeaps->GetPartitionGpuHandle(PartitionType::Texture, slot, m_heapTag);

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
// CPU SRV → GPU SRV（供 SsaoRenderer 绑定使用）
// ========================================================================

D3D12_GPU_DESCRIPTOR_HANDLE AmbientOcclusionManager::CpuSrvToGpu(D3D12_CPU_DESCRIPTOR_HANDLE cpuHandle) const {
    if (cpuHandle.ptr == 0 || !m_descriptorHeaps)
        return {};

    // 注意：必须显式传 m_heapTag（规则 17）——AO 资源在 Editor 多堆模式下位于
    // EditorViewport 堆，若用默认 Default 堆基址计算偏移，会得到跨堆垃圾句柄
    // （LightingRenderer 绑定 ssaoSrv 时 GBV #646 INVALID_DESCRIPTOR_HANDLE，句柄 0x8000...）。
    ID3D12DescriptorHeap *heap = m_descriptorHeaps->GetHeap(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, m_heapTag);
    if (!heap)
        return {};

    D3D12_CPU_DESCRIPTOR_HANDLE cpuBase = heap->GetCPUDescriptorHandleForHeapStart();
    D3D12_GPU_DESCRIPTOR_HANDLE gpuBase = heap->GetGPUDescriptorHandleForHeapStart();
    UINT descSize = m_descriptorHeaps->GetDescriptorSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, m_heapTag);

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

ID3D12Resource *AmbientOcclusionManager::GetAmbientResource0() const {
    return m_ambientRT0.IsValid() ? RenderTargetPool::GetInstance().GetResource(m_ambientRT0) : nullptr;
}

ID3D12Resource *AmbientOcclusionManager::GetAmbientResource1() const {
    return m_ambientRT1.IsValid() ? RenderTargetPool::GetInstance().GetResource(m_ambientRT1) : nullptr;
}

// ========================================================================
// AO 计算入口
// ========================================================================

void AmbientOcclusionManager::Execute(ID3D12GraphicsCommandList *cmdList, D3D12_GPU_DESCRIPTOR_HANDLE depthSRV,
                                      D3D12_GPU_DESCRIPTOR_HANDLE normalSRV, const DirectX::XMFLOAT4X4 &view,
                                      const DirectX::XMFLOAT4X4 &proj) {
    if (!m_initialized || !cmdList)
        return;

    // 包装为 CommandList（与 SsaoRenderer 接口匹配）
    CommandList wrappedCmdList(cmdList);

    // 获取当前的 AO PSO 和模糊 PSO
    ID3D12PipelineState *aoPSO = GetCurrentPSO();
    ID3D12PipelineState *blurPSO = m_ssaoRenderer.GetBlurPipeline();

    // 执行 SSAO 管线
    m_ssaoRenderer.Execute(wrappedCmdList, aoPSO, blurPSO, depthSRV,
                           normalSRV,             // normalSRV（来自 G-buffer）
                           GetAmbientMapSRV(),    // ambientSRV
                           GetAmbientMapRTV(),    // ambientRTV
                           GetAmbientMap1SRV(),   // ambient1SRV
                           GetAmbientMap1RTV(),   // ambient1RTV
                           GetAmbientResource0(), // ambientRes0
                           GetAmbientResource1(), // ambientRes1
                           view,                  // view 矩阵（法线 World→View）
                           proj);                 // proj 矩阵
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

void AmbientOcclusionManager::OnResize(uint32_t width, uint32_t height) {
    if (!m_initialized || width == 0 || height == 0)
        return;
    if (m_renderWidth == width && m_renderHeight == height)
        return;
    ReleaseResources();
    BuildResources(width, height);
}

} // namespace DX12Engine::Renderer
