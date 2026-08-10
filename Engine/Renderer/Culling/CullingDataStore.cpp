#include "CullingDataStore.h"

#include <cstdio> // std::snprintf（[Diag] 桶偏移表抽样）

using namespace DX12Engine::Renderer; // InstanceData（160B 单实例缓冲，FrameResourceTypes.h）

// ========================================================================
// CullingDataStore 实现（P1 迁移，2026-08-10）
//
// 迁移自 InstanceCullingBuffer::SetFlatInstances（InstanceCullingBuffer.cpp:112-214）
// 的数据部分：四字段赋值 + 桶偏移表前缀和 + 诊断日志。
// 旧系统（InstanceCullingBuffer）保留作参考，稳定后移除。
// ========================================================================

namespace DX12Engine {
namespace Culling {

uint32_t CullingDataStore::SetFlatInstances(D3D12_GPU_VIRTUAL_ADDRESS instanceSegmentAddr, ID3D12Resource *instanceRes,
                                            uint32_t instanceCount, std::vector<uint32_t> &&bucketMap) {
    // 单实例缓冲合并（2026-08-09 第三步）：实例数据由 FrameSync 统一上传到 "Instance" 段
    // （InstanceData 160B，含剔除 meta）——CS 的 gInstances 绑定该段（不再独立上传 GPUInstanceData）。
    m_instanceSegmentAddr = instanceSegmentAddr;
    m_instanceRes = instanceRes;
    m_bucketMap = std::move(bucketMap);
    m_instanceCount = instanceCount; // 实例数由 Editor allInstances.size() 决定（CS SRV NumElements）

    // 容量需求：实例桶引用数（Σ bucketCount = m_bucketMap.size()）超过 AppendBuffer 容量时扩容。
    // 方案 B 后 CS 对每个可见实例的每个材质段桶各写一条 gAppend（无 kMaxBucketsPerEntity 上限），
    // 写入量 = Σ(可见实例×桶数) ≤ m_bucketMap.size()，可远超 m_instanceCount（实体数）——
    // 按实体数分配会越界写（GBV #961 "Root descriptor access out of bounds"）。
    // 数据层只计算需求，扩容由资源层（CullingResourceManager::ResizeAppendBuffer）执行——门面协调。
    const uint32_t appendNeed = std::max<uint32_t>(m_instanceCount, static_cast<uint32_t>(m_bucketMap.size()));

    ComputeBucketOffsets();

    // L2c 扁平化实例统计（节流 120 帧——FrameSync 每帧调用 SetFlatInstances，Info 直接打会刷屏）
    static uint32_t s_flatDiagFrame = 0;
    if ((++s_flatDiagFrame % 120) == 1) {
        uint32_t usedBuckets = 0;
        for (uint32_t b = 0; b < kMaxCullBuckets; ++b) {
            const uint32_t next = m_bucketOffsets[b + 1];
            if (next > m_bucketOffsets[b])
                ++usedBuckets;
        }
        Logger::Logger::GetInstance()->Info("[CullingDataStore] L2c flat instances: {} ({}B/instance), buckets={}",
                                            m_instanceCount, sizeof(InstanceData), usedBuckets);
    }
    return appendNeed;
}

const std::vector<uint32_t> &CullingDataStore::ComputeBucketOffsets() {
    // L2c 桶偏移表（前缀和）：bucketOffsets[b] = 桶 b 在 AppendBuffer 的起始实例偏移，
    // 供 CS Append 分段（gAppend[bucketOffset[b] + slot]）。统计各桶实例数（实体级：每实体的每个材质段桶各计 1），
    // 越界桶（>=kMaxCullBuckets）归入桶 0
    // 方案 B：桶归属来自扁平映射表 m_bucketMap（每实体 [bucketOffset, bucketOffset+bucketCount)），
    // 不再有 kMaxBucketsPerEntity 固定上限（无截断）
    // 修复（2026-08-10 卡死根因）：此前遍历 m_cpuInstances 读 inst.meta.y/meta.z——单实例缓冲合并
    // （第三步）后 m_cpuInstances 由 CollectFromBlocks 填充、meta.y/meta.z 从未赋值（未初始化栈垃圾，
    // meta.z 巨大值 → for(k=0;k<n;++k) 天文循环卡死）。改为直接遍历 m_bucketMap 全表统计
    // 各桶出现次数（每实体每桶在扁平表中恰好一条 = 实体级计数语义一致，且不依赖垃圾 meta 字段）。
    m_bucketOffsets.assign(kMaxCullBuckets + 1, 0u);
    for (const uint32_t bidx : m_bucketMap) {
        const uint32_t b = (bidx < kMaxCullBuckets) ? bidx : 0u; // 越界桶归入桶 0（防御）
        ++m_bucketOffsets[b];
    }
    uint32_t acc = 0;
    uint32_t usedBuckets = 0;
    for (uint32_t b = 0; b < kMaxCullBuckets; ++b) {
        const uint32_t cnt = m_bucketOffsets[b];
        m_bucketOffsets[b] = acc;
        acc += cnt;
        if (cnt > 0)
            ++usedBuckets;
    }
    m_bucketOffsets[kMaxCullBuckets] = acc; // 末尾 = 总实例数

    // [Diag] 桶偏移表抽样（节流 120 帧，验证前缀和与 CS 分段基址一致）：
    // 打印 usedBuckets（非空桶数）、总实例数 acc、前 6 个非空桶的 {bucketIndex → 段基址偏移}。
    {
        static uint32_t s_offsetsDiagFrame = 0;
        if ((++s_offsetsDiagFrame % 120) == 1) {
            char firstBuckets[256] = {};
            int written = 0;
            for (uint32_t b = 0; b < kMaxCullBuckets && written < 6; ++b) {
                const uint32_t next = (b + 1 <= kMaxCullBuckets) ? m_bucketOffsets[b + 1] : acc;
                if (next > m_bucketOffsets[b]) {
                    written += std::snprintf(firstBuckets + written, sizeof(firstBuckets) - written, " [%u→%u]", b,
                                             m_bucketOffsets[b]);
                }
            }
            Logger::Logger::GetInstance()->Info("[CullingDataStore][Diag] bucketOffsets: usedBuckets={} total={}{}",
                                                usedBuckets, acc, firstBuckets);
        }
    }
    return m_bucketOffsets;
}

void CullingDataStore::SetCullData(std::vector<CullData> &&cullData) { m_cullData = std::move(cullData); }

void CullingDataStore::Clear() {
    m_instanceSegmentAddr = 0;
    m_instanceRes = nullptr;
    m_instanceCount = 0;
    m_bucketMap.clear();
    m_bucketOffsets.clear();
    m_cullData.clear();
}

} // namespace Culling
} // namespace DX12Engine
