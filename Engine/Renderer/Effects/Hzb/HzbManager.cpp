#include "HzbManager.h"

#include "Logger/Logger.h"
#include "Renderer/RHI/Command/CommandList/CommandList.h"
#include "Renderer/RHI/Command/CommandManager.h"
#include "Resource/Core/DescriptorHeapCollection.h"
#include "Resource/GpuResourceManager.h"
#include <d3dx12.h>

using namespace DX12Engine::Resource;

namespace DX12Engine {
namespace Renderer {

// HZB mip 链纹理格式：R32_FLOAT（深度降采样，单通道）
static constexpr DXGI_FORMAT kHzbFormat = DXGI_FORMAT_R32_FLOAT;

// ========================================================================
// 单例（对齐 AmbientOcclusionManager::GetInstance）
// ========================================================================

HzbManager &HzbManager::GetInstance() {
    static HzbManager s_instance;
    return s_instance;
}

// ========================================================================
// 生命周期
// ========================================================================

void HzbManager::Initialize(ID3D12Device *device, Resource::DescriptorHeapCollection *descriptorHeaps,
                            uint32_t renderWidth, uint32_t renderHeight, Resource::HeapTag heapTag,
                            CommandManager *cmdMgr) {
    if (m_initialized) {
        Shutdown();
    }

    m_device = device;
    m_descriptorHeaps = descriptorHeaps;
    m_heapTag = heapTag;
    m_cmdMgr = cmdMgr;

    if (!m_device || !m_descriptorHeaps) {
        return;
    }

    // HZB 是基础数据，默认生成（无开关）——与 AO 的 m_enabled 区别
    BuildResources(renderWidth, renderHeight);

    // 初始化 HZB 渲染器（device + 描述符堆 + 堆域标签——规则 17）
    m_renderer.SetDevice(m_device);
    m_renderer.SetDescriptorHeaps(m_descriptorHeaps);
    m_renderer.SetHeapTag(m_heapTag);
    if (!m_renderer.Initialize()) {
        Logger::Logger::GetInstance()->Error("[HzbManager] HzbRenderer initialize failed");
        return;
    }

    m_initialized = true;
    Logger::Logger::GetInstance()->Info("[HzbManager] initialized: {}x{} mipCount={} format=R32_FLOAT", m_renderWidth,
                                        m_renderHeight, m_mipCount);
}

void HzbManager::Shutdown() {
    if (!m_initialized)
        return;

    m_renderer.Shutdown();
    ReleaseResources();

    m_device = nullptr;
    m_descriptorHeaps = nullptr;
    m_cmdMgr = nullptr;
    m_initialized = false;
}

// ========================================================================
// 窗口缩放（尺寸比对防重复重建，规则 9）
// ========================================================================

void HzbManager::OnResize(uint32_t width, uint32_t height) {
    if (!m_initialized || width == 0 || height == 0)
        return;
    if (m_renderWidth == width && m_renderHeight == height)
        return;
    ReleaseResources();
    BuildResources(width, height);
}

// ========================================================================
// 资源管理
// ========================================================================

void HzbManager::BuildResources(uint32_t width, uint32_t height) {
    if (width == 0 || height == 0)
        return;

    m_mipCount = ComputeMipCount(width, height);

    // ── HZB mip 链纹理（R32_FLOAT，ALLOW_UNORDERED_ACCESS，初始 COMMON） ──
    D3D12_RESOURCE_DESC resDesc = {};
    resDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    resDesc.Width = width;
    resDesc.Height = height;
    resDesc.DepthOrArraySize = 1;
    resDesc.MipLevels = m_mipCount;
    resDesc.Format = kHzbFormat;
    resDesc.SampleDesc.Count = 1;
    resDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

    CD3DX12_HEAP_PROPERTIES defaultHeap(D3D12_HEAP_TYPE_DEFAULT);
    HRESULT hr = m_device->CreateCommittedResource(&defaultHeap, D3D12_HEAP_FLAG_NONE, &resDesc,
                                                   D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&m_hzbTexture));
    if (FAILED(hr) || !m_hzbTexture) {
        Logger::Logger::GetInstance()->Error("[HzbManager] HZB texture CreateCommittedResource failed hr=0x{:X}", hr);
        return;
    }
    m_hzbTexture->SetName(L"Hzb_MipChain");

    // ── 全链 SRV（消费方采样：MostDetailedMip=0，MipLevels=mipCount） ──
    m_srvSlot = m_descriptorHeaps->Allocate(m_heapTag, PartitionType::Texture);
    if (m_srvSlot == UINT32_MAX) {
        Logger::Logger::GetInstance()->Error("[HzbManager] HZB SRV slot Allocate failed");
        return;
    }
    D3D12_CPU_DESCRIPTOR_HANDLE srvCpu =
        m_descriptorHeaps->GetPartitionCpuHandle(PartitionType::Texture, m_srvSlot, m_heapTag);
    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Format = kHzbFormat;
    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.Texture2D.MostDetailedMip = 0;
    srvDesc.Texture2D.MipLevels = m_mipCount;
    m_device->CreateShaderResourceView(m_hzbTexture.Get(), &srvDesc, srvCpu);

    // GPU handle = CPU handle - heapStartCPU + heapStartGPU（同一描述符堆 CPU/GPU 转换）
    auto *heap = m_descriptorHeaps->GetHeap(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, m_heapTag);
    if (heap) {
        D3D12_CPU_DESCRIPTOR_HANDLE cpuStart = heap->GetCPUDescriptorHandleForHeapStart();
        D3D12_GPU_DESCRIPTOR_HANDLE gpuStart = heap->GetGPUDescriptorHandleForHeapStart();
        m_srvHandle.ptr = srvCpu.ptr - cpuStart.ptr + gpuStart.ptr;
    } else {
        m_srvHandle = m_descriptorHeaps->GetPartitionGpuHandle(PartitionType::Texture, m_srvSlot, m_heapTag);
    }

    // ── 每级 mip UAV（[0..mipCount-1]，连续槽位）——mip0 = 深度图 1:1 拷贝（标准 HZB：
    //   mip0 是有效层级，与窗口大小匹配，遮挡测试从 mip0 起选层采样） ──
    // 级数：mip0..mip(mipCount-1) 共 mipCount 个 UAV
    m_uavBaseSlot = m_descriptorHeaps->AllocateConsecutive(m_heapTag, PartitionType::Texture, m_mipCount);
    if (m_uavBaseSlot == UINT32_MAX) {
        Logger::Logger::GetInstance()->Error("[HzbManager] HZB UAV slots AllocateConsecutive failed (need {})",
                                             m_mipCount);
        return;
    }
    m_mipUAVs.clear();
    m_mipUAVs.reserve(m_mipCount);
    for (uint32_t i = 0; i < m_mipCount; ++i) {
        uint32_t slot = m_uavBaseSlot + i;
        D3D12_CPU_DESCRIPTOR_HANDLE uavCpu =
            m_descriptorHeaps->GetPartitionCpuHandle(PartitionType::Texture, slot, m_heapTag);
        D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
        uavDesc.Format = kHzbFormat;
        uavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        uavDesc.Texture2D.MipSlice = i; // 每级绑定对应 mip slice
        // CreateUnorderedAccessView(pResource, pCounterResource=nullptr, pDesc, DestDescriptor)——4 参签名
        m_device->CreateUnorderedAccessView(m_hzbTexture.Get(), nullptr, &uavDesc, uavCpu);

        D3D12_GPU_DESCRIPTOR_HANDLE uavGpu =
            m_descriptorHeaps->GetPartitionGpuHandle(PartitionType::Texture, slot, m_heapTag);
        m_mipUAVs.push_back(uavGpu);
    }

    m_renderWidth = width;
    m_renderHeight = height;

    // 创建后同步初始化 HZB 内容为远值 1.0（无效默认值纹理——首帧/场景切换兜底：
    // HZB 未被任何一帧构建时内容 = 1.0（远）→ 遮挡测试 objNear < 1.0 不剔 → 天然不误剔）
    InitializeContentToFar();
}

// ========================================================================
// HZB 内容初始化（远值 1.0——无效默认值纹理，对齐 BlankTextureProvider 回退模式）
// ========================================================================

void HzbManager::InitializeContentToFar() {
    // cmdMgr 缺失（Game 端未传）时跳过——调用方保证此时不消费 HZB
    if (!m_cmdMgr || !m_hzbTexture || m_mipUAVs.empty() || !m_device)
        return;

    auto &gpuMgr = Resource::GpuResourceManager::GetInstance();

    // ── 上传数据构造：每级 mip 填 1.0（R32_FLOAT 远值）——对齐 BlankTextureProvider 上传模式 ──
    // 2026-08-13 弃用 ClearUnorderedAccessViewFloat：其 CPU handle 必须指向 CPU-only 堆
    // （shader-visible 堆的 CPU 视图是 CPU-write-only，驱动读取无效 → #646 INVALID_DESCRIPTOR_HANDLE）。
    // 改为 UpdateSubresources（COPY_DEST 上传，BlankTextureProvider::CreateWhite2D 已验证模式）。
    std::vector<D3D12_SUBRESOURCE_DATA> subDatas(m_mipCount);
    std::vector<std::vector<float>> mipData(m_mipCount);
    for (uint32_t i = 0; i < m_mipCount; ++i) {
        const uint32_t w = std::max(1u, m_renderWidth >> i);
        const uint32_t h = std::max(1u, m_renderHeight >> i);
        mipData[i].assign(static_cast<size_t>(w) * h, 1.0f); // R32_FLOAT 远值 1.0
        subDatas[i].pData = mipData[i].data();
        subDatas[i].RowPitch = w * sizeof(float);
        subDatas[i].SlicePitch = w * h * sizeof(float);
    }
    UINT64 uploadSize = GetRequiredIntermediateSize(m_hzbTexture.Get(), 0, m_mipCount);
    Resource::GpuResourceHandle uploadBuf =
        gpuMgr.CreateBuffer(m_device, static_cast<uint32_t>(uploadSize), L"HzbManager_InitToFar_Up",
                            D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
    if (!uploadBuf.IsValid())
        return;

    const uint64_t completedFence = m_cmdMgr->GetCompletedFenceValue(D3D12_COMMAND_LIST_TYPE_DIRECT);
    auto allocH = m_cmdMgr->AcquireAllocator<D3D12_COMMAND_LIST_TYPE_DIRECT>(completedFence);
    auto *alloc = m_cmdMgr->GetAllocator<D3D12_COMMAND_LIST_TYPE_DIRECT>(allocH);
    auto cmdH = m_cmdMgr->AcquireCommandListHandle<D3D12_COMMAND_LIST_TYPE_DIRECT>(alloc);
    auto cmdList = m_cmdMgr->GetCommandList<D3D12_COMMAND_LIST_TYPE_DIRECT>(cmdH);

    // 入口屏障：COMMON → COPY_DEST（规则 10 对称——UpdateSubresources 要求 COPY_DEST）
    auto barrierToCopy = CD3DX12_RESOURCE_BARRIER::Transition(m_hzbTexture.Get(), D3D12_RESOURCE_STATE_COMMON,
                                                              D3D12_RESOURCE_STATE_COPY_DEST);
    cmdList.Get()->ResourceBarrier(1, &barrierToCopy);

    // 上传全 mip 链（1.0 远值）
    UpdateSubresources(cmdList.Get(), m_hzbTexture.Get(), gpuMgr.GetResource(uploadBuf), 0, 0, m_mipCount,
                       subDatas.data());

    // 出口屏障：COPY_DEST → COMMON（规则 10 对称恢复）
    auto barrierBack = CD3DX12_RESOURCE_BARRIER::Transition(m_hzbTexture.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
                                                            D3D12_RESOURCE_STATE_COMMON);
    cmdList.Get()->ResourceBarrier(1, &barrierBack);

    cmdList.Close();
    m_cmdMgr->Submit(D3D12_COMMAND_LIST_TYPE_DIRECT, cmdList);
    m_cmdMgr->Flush(D3D12_COMMAND_LIST_TYPE_DIRECT); // 同步阻塞至 GPU 完成（对齐 BlankTextureProvider）

    const uint64_t seq = m_cmdMgr->GetNextSequence();
    m_cmdMgr->ReleaseAllocator<D3D12_COMMAND_LIST_TYPE_DIRECT>(allocH, seq);
    gpuMgr.Release(uploadBuf, seq);
}

void HzbManager::ReleaseResources() {
    if (m_hzbTexture) {
        m_hzbTexture.Reset();
    }
    if (m_descriptorHeaps) {
        if (m_srvSlot != UINT32_MAX) {
            m_descriptorHeaps->Free(m_heapTag, PartitionType::Texture, m_srvSlot, UINT64_MAX);
            m_srvSlot = UINT32_MAX;
        }
        if (m_uavBaseSlot != UINT32_MAX) {
            for (uint32_t i = 0; i < m_mipCount; ++i) {
                m_descriptorHeaps->Free(m_heapTag, PartitionType::Texture, m_uavBaseSlot + i, UINT64_MAX);
            }
            m_uavBaseSlot = UINT32_MAX;
        }
    }
    m_mipUAVs.clear();
    m_srvHandle = {};
    m_renderWidth = 0;
    m_renderHeight = 0;
    m_mipCount = 0;
}

// ========================================================================
// HZB 构建入口（HZB_Build 阶段录制）
// ========================================================================

void HzbManager::Execute(CommandList &cmd, ID3D12Resource *depthRes, D3D12_GPU_DESCRIPTOR_HANDLE depthSRV) {
    if (!m_initialized || !depthRes || depthSRV.ptr == 0)
        return;
    if (!m_hzbTexture || m_mipUAVs.empty())
        return;
    m_renderer.Execute(cmd, depthRes, depthSRV, m_hzbTexture.Get(), m_mipUAVs, m_mipCount, m_renderWidth,
                       m_renderHeight);
}

} // namespace Renderer
} // namespace DX12Engine
