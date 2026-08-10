#include "CullingResourceManager.h"

#include "Logger/Logger.h"
#include <algorithm> // std::max
#include <cstring>   // memset

using namespace DX12Engine::Renderer;
using namespace DX12Engine::Resource;

// ========================================================================
// CullingResourceManager 实现（P2 迁移，2026-08-10）
//
// 迁移自 InstanceCullingBuffer（InstanceCullingBuffer.cpp）：
//   ResizeAppendBuffer（221-260）/ CreateUAVs（469-629）/ ReleaseCullingResources（406-467）
//   + 资源字段（m_appendBuffer/m_indirectArgs/m_bucketOffsetsUp/m_bucketArgsUp/
//   m_zeroUpload/m_cullParamsUp/m_visibleReadback + UAV 槽位）。
// 旧系统（InstanceCullingBuffer）保留作参考，稳定后移除。
// ========================================================================

namespace DX12Engine {
namespace Culling {

void CullingResourceManager::Initialize(ID3D12Device *device, Resource::DescriptorHeapCollection *descHeaps,
                                        Resource::HeapTag heapTag) {
    m_device = device;
    m_descHeaps = descHeaps;
    m_heapTag = heapTag;
    m_initialized = (device != nullptr && descHeaps != nullptr);
}

void CullingResourceManager::Shutdown() {
    ReleaseCullingResources();
    m_device = nullptr;
    m_descHeaps = nullptr;
    m_initialized = false;
}

bool CullingResourceManager::CreateUAVs(uint32_t instanceCount, const std::vector<uint32_t> &bucketMap) {
    if (!m_initialized || !m_device || !m_descHeaps || instanceCount == 0)
        return false;

    auto &gpuMgr = Resource::GpuResourceManager::GetInstance();

    // [CullDiag] 重建时先释放旧资源 + 旧 UAV 槽位（统一经 ReleaseCullingResources——
    // 对齐 CreateSRV 的 Free 旧槽位模式，防止场景重建多次 CreateCullingPipeline → 句柄覆盖泄漏 + 槽位膨胀）
    ReleaseCullingResources();

    // AppendBuffer：RWStructuredBuffer<uint>（容量 = max(实例数, Σ桶引用)，DEFAULT + 初始 NON_PIXEL_SHADER_RESOURCE）
    // UAV 资源必须带 D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS（否则 #524）
    // 状态设计（规则 10 对称屏障）：创建/VS 消费后停在 SRV；DispatchCulling 入口 SRV→UAV、
    // 出口 UAV→SRV（CS 以 UAV 写、VS color.hlsl:83 以 SRV 读 gAppendBuffer——GBV #942 修复）
    // 方案 B：CS 写入量 = Σ(可见实例×桶数) ≤ bucketMap.size()，可远超 instanceCount——
    // 容量必须按 max(实体数, Σ桶引用) 分配，否则 gAppend 越界写（GBV #961）
    // 预分配余量 ×2（2026-08-09 静止帧率骤降修复）：场景加载中 bucketMap 逐渐增长（异步加载），
    // 初始按当前值分配会在加载期触发 ResizeAppendBuffer 重建 → GPU 同步阻塞 → 帧率 12-15FPS。
    // ×2 余量覆盖增长，避免运行时扩容（大场景仍需 ResizeAppendBuffer，但其内也加余量）。
    const uint32_t appendNeed = (std::max)(instanceCount, static_cast<uint32_t>(bucketMap.size())) * 2u;
    const uint32_t appendSize = appendNeed * sizeof(uint32_t);
    m_appendCapacity = appendSize; // 记录容量（SetFlatInstances 实例数增长超限时重建扩容，防 gAppend 越界写）
    m_appendBuffer =
        gpuMgr.CreateBuffer(m_device, appendSize, L"InstanceCulling_Append", D3D12_HEAP_TYPE_DEFAULT,
                            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    if (!m_appendBuffer.IsValid())
        return false;

    // IndirectArgs：前部 = 每桶 kMaxSubMeshRanges 段 × D3D12_DRAW_INDEXED_ARGUMENTS（5
    // uint：IndexCount/InstanceCount/StartIndex/BaseVertex/StartInstance），
    // 定案 7.2a：同材质多子网格段聚合一桶，ExecuteIndirect MaxCommandCount=段数 一次提交多段；
    // 尾部 = 桶偏移表前缀和（kMaxCullBuckets+1 uint，CS 读取 gIndirectArgs[kMaxCullBuckets*5*kMaxSubMeshRanges +
    // bucket] 作为 gAppend 分段基址，CPU 每帧 COPY 写入）。
    // 空桶跳过（2026-08-09）：尾部再追加非零桶区——[0]=非零桶计数(1 uint) + [1..kMaxCullBuckets]=非零桶索引列表。
    // 地址：kNonZeroCountAddr = kMaxCullBuckets*5*kMaxSubMeshRanges + (kMaxCullBuckets+1)，列表紧随其后，
    // total 在列表末尾（kNonZeroTotalAddr = kNonZeroCountAddr + kMaxCullBuckets + 1）。
    // 布局总量 = 段区 + 桶偏移表(kMaxCullBuckets+1) + 非零桶区(kMaxCullBuckets+2：计数1+列表kMaxCullBuckets+total1)
    const uint32_t indirectArgUints =
        5 * RenderItemCommon::kMaxSubMeshRanges * kMaxCullBuckets + (kMaxCullBuckets + 1) + (kMaxCullBuckets + 2);
    m_indirectArgs = gpuMgr.CreateBuffer(m_device, sizeof(uint32_t) * indirectArgUints, L"InstanceCulling_IndirectArgs",
                                         D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT,
                                         D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    if (!m_indirectArgs.IsValid()) {
        gpuMgr.Release(m_appendBuffer, 0);
        m_appendBuffer = {};
        return false;
    }

    // 桶偏移表 UPLOAD 缓冲（每帧 SetFlatInstances 填充前缀和 → DispatchCulling COPY 到 gIndirectArgs 尾部）
    m_bucketOffsetsUp =
        gpuMgr.CreateBuffer(m_device, sizeof(uint32_t) * (kMaxCullBuckets + 1), L"InstanceCulling_BucketOffsets",
                            D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
    if (!m_bucketOffsetsUp.IsValid()) {
        gpuMgr.Release(m_appendBuffer, 0);
        gpuMgr.Release(m_indirectArgs, 0);
        m_appendBuffer = {};
        m_indirectArgs = {};
        return false;
    }

    // 每桶 DRAW_INDEXED_ARGUMENTS 静态字段 UPLOAD 缓冲（每桶 kMaxSubMeshRanges 段 × 5 uint；ExecuteIndirect 消费，
    // DEFAULT 堆未初始化内存被当垃圾参数会绘制错误——SetBucketDrawArgs 每帧 Map 填充，DispatchCulling COPY）
    m_bucketArgsUp =
        gpuMgr.CreateBuffer(m_device, sizeof(uint32_t) * 5 * RenderItemCommon::kMaxSubMeshRanges * kMaxCullBuckets,
                            L"InstanceCulling_BucketArgs", D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
    if (!m_bucketArgsUp.IsValid()) {
        gpuMgr.Release(m_appendBuffer, 0);
        gpuMgr.Release(m_indirectArgs, 0);
        gpuMgr.Release(m_bucketOffsetsUp, 0);
        m_appendBuffer = {};
        m_indirectArgs = {};
        m_bucketOffsetsUp = {};
        return false;
    }

    // 每帧清零 InstanceCount 用的 UPLOAD 零缓冲（4 字节）
    m_zeroUpload = gpuMgr.CreateBuffer(m_device, sizeof(uint32_t), L"InstanceCulling_Zero", D3D12_HEAP_TYPE_UPLOAD,
                                       D3D12_RESOURCE_STATE_GENERIC_READ);
    if (!m_zeroUpload.IsValid()) {
        gpuMgr.Release(m_appendBuffer, 0);
        gpuMgr.Release(m_indirectArgs, 0);
        m_appendBuffer = {};
        m_indirectArgs = {};
        return false;
    }
    {
        auto *zeroRes = gpuMgr.GetResource(m_zeroUpload);
        void *mapped = nullptr;
        zeroRes->Map(0, nullptr, &mapped);
        memset(mapped, 0, sizeof(uint32_t));
        zeroRes->Unmap(0, nullptr);
    }

    // 验证 readback：dispatch 后把非零桶区复制到 READBACK 堆（延迟 1 帧 CPU 读回求和，
    // 验证 GPU 剔除链路正确性——树先走实体渲染，此缓冲仅统计）。
    // 空桶跳过（2026-08-09）：readback 缓冲 = [0]=非零桶计数 + [1..kMaxCullBuckets]=非零桶索引列表 + [末尾]=total
    // 注意：必须用 D3D12_HEAP_TYPE_READBACK（GPU 写→CPU 读），不能用 UPLOAD——
    // UPLOAD 堆只能 GENERIC_READ，CopyBufferRegion 目标要求 COPY_DEST，barrier 会触发
    // D3D12 #741 RESOURCE_BARRIER_INVALID_HEAP
    m_visibleReadback =
        gpuMgr.CreateBuffer(m_device, sizeof(uint32_t) * (kMaxCullBuckets + 2), L"InstanceCulling_VisibleReadback",
                            D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
    if (!m_visibleReadback.IsValid()) {
        gpuMgr.Release(m_appendBuffer, 0);
        gpuMgr.Release(m_indirectArgs, 0);
        gpuMgr.Release(m_zeroUpload, 0);
        m_appendBuffer = {};
        m_indirectArgs = {};
        m_zeroUpload = {};
        return false;
    }
    // 初始清零（2026-08-09 空桶跳过修复）：相机静止帧 dispatch 跳过 → readback 不 COPY →
    // 若未清零，ReadbackNonZeroBucketList 读到 READBACK 堆未定义垃圾计数 → 空桶跳过误删本应绘制的桶。
    {
        auto *rbRes = gpuMgr.GetResource(m_visibleReadback);
        if (rbRes) {
            void *rbMapped = nullptr;
            if (SUCCEEDED(rbRes->Map(0, nullptr, &rbMapped)) && rbMapped) {
                memset(rbMapped, 0, sizeof(uint32_t) * (kMaxCullBuckets + 2));
                rbRes->Unmap(0, nullptr);
            }
        }
    }

    // UAV 描述符（Buffer 分区，与实例 SRV 同堆）
    m_appendUavIndex = m_descHeaps->Allocate(m_heapTag, PartitionType::Buffer);
    m_indirectArgsUavIndex = m_descHeaps->Allocate(m_heapTag, PartitionType::Buffer);
    if (m_appendUavIndex == UINT32_MAX || m_indirectArgsUavIndex == UINT32_MAX) {
        Logger::Logger::GetInstance()->Error("[CullingResourceManager] Buffer UAV partition exhausted");
        return false;
    }

    D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
    uavDesc.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
    uavDesc.Buffer.FirstElement = 0;
    // 方案 B：容量 = max(实体数, Σ桶引用)——UAV NumElements 必须与资源容量一致，否则 gAppend 越界（GBV #961）
    uavDesc.Buffer.NumElements = appendNeed;
    uavDesc.Buffer.StructureByteStride = sizeof(uint32_t); // RWStructuredBuffer<uint>
    auto appendCpu = m_descHeaps->GetPartitionCpuHandle(PartitionType::Buffer, m_appendUavIndex, m_heapTag);
    m_device->CreateUnorderedAccessView(gpuMgr.GetResource(m_appendBuffer), nullptr, &uavDesc, appendCpu);

    D3D12_UNORDERED_ACCESS_VIEW_DESC argsUav{};
    argsUav.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
    argsUav.Buffer.FirstElement = 0;
    // 前部 DRAW_INDEXED_ARGUMENTS×段数×桶数 + 尾部桶偏移表 + 非零桶区(计数+列表+total，空桶跳过 2026-08-09)
    argsUav.Buffer.NumElements =
        5 * RenderItemCommon::kMaxSubMeshRanges * kMaxCullBuckets + (kMaxCullBuckets + 1) + (kMaxCullBuckets + 2);
    argsUav.Buffer.StructureByteStride = sizeof(uint32_t);
    auto argsCpu = m_descHeaps->GetPartitionCpuHandle(PartitionType::Buffer, m_indirectArgsUavIndex, m_heapTag);
    m_device->CreateUnorderedAccessView(gpuMgr.GetResource(m_indirectArgs), nullptr, &argsUav, argsCpu);

    Logger::Logger::GetInstance()->Info("[CullingResourceManager] L2b UAVs created: append(slot={}, {}B) args(slot={})",
                                        m_appendUavIndex, appendSize, m_indirectArgsUavIndex);
    return true;
}

bool CullingResourceManager::CreateCullDataResources() {
    if (!m_initialized || !m_device)
        return false;
    auto &gpuMgr = Resource::GpuResourceManager::GetInstance();

    // CullData 自管 RingBuffer（2026-08-10 剥离：不再经 FrameResourceManager 帧段——
    // 规避 fence 回收依赖/共享 RingBuffer 竞争，类比 LightManager 自管缓冲；生命周期自管）。
    // UPLOAD 堆，固定容量 kCullDataCapacity，段地址固定（每帧 Map 重写，消除频闪竞态）
    if (!m_cullDataBuffer.IsValid()) {
        m_cullDataBuffer = gpuMgr.CreateBuffer(m_device, kCullDataCapacity, L"Culling_CullData", D3D12_HEAP_TYPE_UPLOAD,
                                               D3D12_RESOURCE_STATE_GENERIC_READ);
        if (!m_cullDataBuffer.IsValid()) {
            Logger::Logger::GetInstance()->Error("[CullingResourceManager] CullData self-managed buffer create failed");
            return false;
        }
        auto *res = gpuMgr.GetResource(m_cullDataBuffer);
        if (res)
            m_cullDataBufferAddr = res->GetGPUVirtualAddress(); // 固定地址（每帧 Map 重写，不滚动）
        Logger::Logger::GetInstance()->Info(
            "[CullingResourceManager] CullData self-managed buffer: capacity={} addr=0x{:x}", kCullDataCapacity,
            static_cast<uint64_t>(m_cullDataBufferAddr));
    }
    return true;
}

void CullingResourceManager::ResizeAppendBuffer(uint32_t instanceCount, const std::vector<uint32_t> &bucketMap,
                                                uint64_t releaseFence) {
    if (!m_device || !m_descHeaps || instanceCount == 0)
        return;
    auto &gpuMgr = Resource::GpuResourceManager::GetInstance();

    // 释放旧 AppendBuffer（UAV 描述符槽位复用，不释放——仍指向新资源）
    // 2026-08-10 #921 修复：必须用 releaseFence（GPU 完成本帧 dispatch 后释放）——
    // 传 0 会立即释放，而旧 AppendBuffer 本帧 dispatch 仍 in-flight → OBJECT_DELETED_WHILE_STILL_IN_USE
    if (m_appendBuffer.IsValid()) {
        gpuMgr.Release(m_appendBuffer, releaseFence);
        m_appendBuffer = {};
    }

    // 按新容量重建（容量 = max(实例数, Σ桶引用) × 4B，DEFAULT + 初始 NON_PIXEL_SHADER_RESOURCE）
    // 状态设计（规则 10 对称屏障）：创建/VS 消费后停在 SRV；DispatchCulling 入口 SRV→UAV、
    // 出口 UAV→SRV（CS 以 UAV 写、VS color.hlsl:83 以 SRV 读 gAppendBuffer——GBV #942 修复）
    // 方案 B：CS 写入量 = Σ(可见实例×桶数) ≤ bucketMap.size()，可远超 instanceCount——
    // 容量必须按 max(实体数, Σ桶引用) 分配，否则 gAppend 越界写（GBV #961）
    const uint32_t appendNeed = std::max<uint32_t>(instanceCount, static_cast<uint32_t>(bucketMap.size()));
    const uint32_t appendSize = appendNeed * sizeof(uint32_t);
    m_appendBuffer =
        gpuMgr.CreateBuffer(m_device, appendSize, L"InstanceCulling_Append", D3D12_HEAP_TYPE_DEFAULT,
                            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    if (!m_appendBuffer.IsValid()) {
        Logger::Logger::GetInstance()->Error(
            "[CullingResourceManager] ResizeAppendBuffer: AppendBuffer recreate failed");
        return;
    }
    m_appendCapacity = appendSize; // 容量随新资源更新

    // 重建 Append UAV 描述符（复用 m_appendUavIndex 槽位；RWStructuredBuffer<uint>）
    D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
    uavDesc.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
    uavDesc.Buffer.FirstElement = 0;
    uavDesc.Buffer.NumElements = appendNeed;
    uavDesc.Buffer.StructureByteStride = sizeof(uint32_t);
    auto appendCpu = m_descHeaps->GetPartitionCpuHandle(PartitionType::Buffer, m_appendUavIndex, m_heapTag);
    m_device->CreateUnorderedAccessView(gpuMgr.GetResource(m_appendBuffer), nullptr, &uavDesc, appendCpu);

    Logger::Logger::GetInstance()->Info("[CullingResourceManager] AppendBuffer resized: {}B ({} needed)",
                                        m_appendCapacity, appendNeed);
}

void CullingResourceManager::SetBucketDrawArgs(uint32_t bucketIndex, uint32_t subMeshCount,
                                               const RenderItemCommon::SubMeshRange *ranges) {
    if (bucketIndex >= kMaxCullBuckets || !m_bucketArgsUp.IsValid())
        return;
    auto *res = Resource::GpuResourceManager::GetInstance().GetResource(m_bucketArgsUp);
    if (!res)
        return;
    // 每桶段区 = 段数 × D3D12_DRAW_INDEXED_ARGUMENTS（5
    // uint：IndexCount/InstanceCount/StartIndex/BaseVertex/StartInstance）。 每段 IndexCount/StartIndex
    // 为静态（本桶几何段），InstanceCount 留 0（compute 对每段原子递增）， BaseVertex 恒 0（§2.3）；未用段清零。
    const uint32_t segs = (subMeshCount > 0 && subMeshCount <= RenderItemCommon::kMaxSubMeshRanges) ? subMeshCount : 1u;
    uint32_t args[RenderItemCommon::kMaxSubMeshRanges * 5] = {};
    for (uint32_t s = 0; s < segs; ++s) {
        args[s * 5 + 0] = (ranges && s < subMeshCount) ? ranges[s].indexCount : 0u; // IndexCount
        args[s * 5 + 1] = 0u;                                                       // InstanceCount（compute 填充）
        args[s * 5 + 2] = (ranges && s < subMeshCount) ? ranges[s].startIndex : 0u; // StartIndex
        args[s * 5 + 3] = 0u;                                                       // BaseVertex
        args[s * 5 + 4] = 0u;                                                       // StartInstance（恒 0）
    }
    void *mapped = nullptr;
    if (SUCCEEDED(res->Map(0, nullptr, &mapped))) {
        memcpy(static_cast<uint32_t *>(mapped) + bucketIndex * 5 * RenderItemCommon::kMaxSubMeshRanges, args,
               sizeof(args));
        res->Unmap(0, nullptr);
    }

    // [Diag] 每桶 DRAW_INDEXED_ARGUMENTS 静态字段抽样（2026-08-10 清理：每帧 178 桶调用
    // 必打一次（120 次调用节流≠120 帧），已移除——桶段参数诊断不再关心，保留注释说明）
    // 若需验证：ExecuteIndirect 消费的就是本缓冲（经 DispatchCulling COPY 到 gIndirectArgs），
    // 与实际网格段不符 → 绘制错乱（RenderDoc 可验证）。
}

void CullingResourceManager::ReleaseCullingResources() {
    auto &gpuMgr = Resource::GpuResourceManager::GetInstance();
    auto *logger = Logger::Logger::GetInstance();
    // 释放配对日志：每个资源释放前打印句柄 index/generation（只读本层句柄字段，不查询分配器内部）。
    // 与 CreateUAVs 的分配日志对照，可确认"分配↔释放"是否成对、释放后句柄是否被置空（IsValid 归 false）
    auto logRelease = [logger](const char *name, Resource::GpuResourceHandle h) {
        const uint32_t raw = static_cast<uint32_t>(h);
        const uint32_t idx = raw & 0x3FFFFFu;      // 低 22 位 index
        const uint32_t gen = (raw >> 22) & 0x3FFu; // 高 10 位 generation
        logger->Info("[CullingResourceManager] Release {}: valid={} handle=0x{:08X} (index={}, generation={})", name,
                     h.IsValid(), raw, idx, gen);
    };
    if (m_appendBuffer.IsValid()) {
        logRelease("AppendBuffer", m_appendBuffer);
        gpuMgr.Release(m_appendBuffer, 0);
        m_appendBuffer = {};
    }
    m_appendCapacity = 0; // 容量随资源释放重置
    if (m_indirectArgs.IsValid()) {
        logRelease("IndirectArgs", m_indirectArgs);
        gpuMgr.Release(m_indirectArgs, 0);
        m_indirectArgs = {};
    }
    if (m_bucketOffsetsUp.IsValid()) {
        logRelease("BucketOffsetsUp", m_bucketOffsetsUp);
        gpuMgr.Release(m_bucketOffsetsUp, 0);
        m_bucketOffsetsUp = {};
    }
    if (m_bucketArgsUp.IsValid()) {
        logRelease("BucketArgsUp", m_bucketArgsUp);
        gpuMgr.Release(m_bucketArgsUp, 0);
        m_bucketArgsUp = {};
    }
    if (m_zeroUpload.IsValid()) {
        logRelease("ZeroUpload", m_zeroUpload);
        gpuMgr.Release(m_zeroUpload, 0);
        m_zeroUpload = {};
    }
    if (m_visibleReadback.IsValid()) {
        logRelease("VisibleReadback", m_visibleReadback);
        gpuMgr.Release(m_visibleReadback, 0);
        m_visibleReadback = {};
    }
    if (m_cullParamsUp.IsValid()) {
        logRelease("CullParams", m_cullParamsUp);
        gpuMgr.Release(m_cullParamsUp, 0);
        m_cullParamsUp = {};
    }
    if (m_cullDataBuffer.IsValid()) {
        logRelease("CullDataBuffer", m_cullDataBuffer);
        gpuMgr.Release(m_cullDataBuffer, 0);
        m_cullDataBuffer = {};
        m_cullDataBufferAddr = 0;
    }
    if (m_descHeaps) {
        if (m_appendUavIndex != UINT32_MAX) {
            m_descHeaps->Free(m_heapTag, PartitionType::Buffer, m_appendUavIndex, UINT64_MAX);
            m_appendUavIndex = UINT32_MAX;
        }
        if (m_indirectArgsUavIndex != UINT32_MAX) {
            m_descHeaps->Free(m_heapTag, PartitionType::Buffer, m_indirectArgsUavIndex, UINT64_MAX);
            m_indirectArgsUavIndex = UINT32_MAX;
        }
    }
}

D3D12_GPU_VIRTUAL_ADDRESS CullingResourceManager::GetAppendBufferAddress() const {
    return m_appendBuffer.IsValid()
               ? Resource::GpuResourceManager::GetInstance().GetResource(m_appendBuffer)->GetGPUVirtualAddress()
               : 0;
}

D3D12_GPU_VIRTUAL_ADDRESS CullingResourceManager::GetIndirectArgsAddress() const {
    return m_indirectArgs.IsValid()
               ? Resource::GpuResourceManager::GetInstance().GetResource(m_indirectArgs)->GetGPUVirtualAddress()
               : 0;
}

ID3D12Resource *CullingResourceManager::GetIndirectArgsResource() const {
    if (!m_indirectArgs.IsValid())
        return nullptr;
    return Resource::GpuResourceManager::GetInstance().GetResource(m_indirectArgs);
}

} // namespace Culling
} // namespace DX12Engine
