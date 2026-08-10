#pragma once

// ========================================================================
// CullingResourceManager — 剔除资源层（GPU 资源生命周期集中管理）
//
// 定案（InstanceCullingSystem.md §六）：
//   资源层 = AppendBuffer/IndirectArgs/CullParams/readback 生命周期管理。
//   对齐 GpuResourceManager 协作模式（规则 11：管理器只管生命周期，
//   状态由使用方对称屏障）；SRV/段/扩容集中管理。
//
// 迁移（P2，2026-08-10）：从 InstanceCullingBuffer 迁出资源句柄/扩容/UAV 槽位
//   （ResizeAppendBuffer / CreateUAVs / ReleaseCullingResources + 资源字段）。
//   旧系统（InstanceCullingBuffer）保留作参考，稳定后移除。
// ========================================================================

#include "Renderer/RenderItemBuilder/RenderItemCommon.h" // kMaxSubMeshRanges
#include "Resource/Core/DescriptorHeapCollection.h"
#include "Resource/GpuResourceManager.h" // GpuResourceHandle
#include <cstdint>
#include <d3d12.h>
#include <vector>

namespace DX12Engine {
namespace Culling {

class CullingResourceManager {
public:
    static constexpr uint32_t kMaxCullBuckets = 1024; // 与 Shaders/InstanceCulling.cs.hlsl 同步

    CullingResourceManager() = default;
    ~CullingResourceManager() = default;

    // ====================================================================
    // 生命周期
    // ====================================================================

    void Initialize(ID3D12Device *device, Resource::DescriptorHeapCollection *descHeaps,
                    Resource::HeapTag heapTag = Resource::HeapTag::Default);
    void Shutdown();

    /// 创建 L2b 剔除资源：AppendBuffer / IndirectArgs / BucketOffsetsUp / BucketArgsUp /
    /// ZeroUpload / VisibleReadback + UAV 槽位（CreateCullingPipeline 调用，场景加载后一次）
    /// @param instanceCount 实例数（数据层提供）
    /// @param bucketMap     扁平桶归属表（数据层提供；容量按 max(实体数, Σ桶引用)×2 余量）
    bool CreateUAVs(uint32_t instanceCount, const std::vector<uint32_t> &bucketMap);

    /// 实例数/桶引用增长超 AppendBuffer 容量时扩容（只重建 AppendBuffer + UAV 描述符，
    /// 不触碰 compute PSO——全量重建会误 Reset 导致 IsCullingReady 失效）
    /// @param releaseFence 旧 AppendBuffer 的延迟释放 fence（GPU 完成本帧 dispatch 后释放——
    /// 传 0 会立即释放 → #921 OBJECT_DELETED_WHILE_STILL_IN_USE（2026-08-10 崩溃修复））
    void ResizeAppendBuffer(uint32_t instanceCount, const std::vector<uint32_t> &bucketMap, uint64_t releaseFence);

    /// 创建 CullData 自管 RingBuffer（2026-08-10 剥离：不再经 FrameResourceManager 帧段——
    /// 规避 fence 回收依赖/共享 RingBuffer 竞争，类比 LightManager 自管缓冲；生命周期自管）。
    /// UPLOAD 堆，固定容量（kCullDataCapacity），段地址固定（每帧 Map 重写，消除频闪竞态）。
    bool CreateCullDataResources();
    Resource::GpuResourceHandle GetCullDataBuffer() const { return m_cullDataBuffer; }
    D3D12_GPU_VIRTUAL_ADDRESS GetCullDataBufferAddr() const { return m_cullDataBufferAddr; }

    /// L2c：预置本桶 DRAW_INDEXED_ARGUMENTS 静态字段（多段：同材质子网格区间聚合，ExecuteIndirect
    /// MaxCommandCount=段数；每段 IndexCount/StartIndex 静态，InstanceCount 留 0 由 compute
    /// 原子递增，BaseVertex=0/StartInstance=0）。
    /// @param bucketIndex 材质槽桶索引（< kMaxCullBuckets）
    /// @param subMeshCount 本桶子网格段数（1..kMaxSubMeshRanges，超限截断）
    /// @param ranges      段列表（IndexCount/StartIndex；为空时用单段兜底）
    void SetBucketDrawArgs(uint32_t bucketIndex, uint32_t subMeshCount,
                           const Renderer::RenderItemCommon::SubMeshRange *ranges = nullptr);

    /// 释放 L2b 全部剔除资源（Append/IndirectArgs/Zero/CullParams/Readback + UAV 槽位）。
    /// Shutdown / CreateUAVs 重建前 / 0 实例场景切换共用（PSO/RootSig 由执行层管理）
    void ReleaseCullingResources();

    // ====================================================================
    // 资源访问（执行层 dispatch / 渲染管线消费）
    // ====================================================================

    D3D12_GPU_VIRTUAL_ADDRESS GetAppendBufferAddress() const;
    D3D12_GPU_VIRTUAL_ADDRESS GetIndirectArgsAddress() const;
    ID3D12Resource *GetIndirectArgsResource() const;
    Resource::GpuResourceHandle GetIndirectArgsHandle() const { return m_indirectArgs; }
    Resource::GpuResourceHandle GetAppendBufferHandle() const { return m_appendBuffer; }
    Resource::GpuResourceHandle GetBucketOffsetsUp() const { return m_bucketOffsetsUp; }
    Resource::GpuResourceHandle GetBucketArgsUp() const { return m_bucketArgsUp; }
    Resource::GpuResourceHandle GetZeroUpload() const { return m_zeroUpload; }
    Resource::GpuResourceHandle GetVisibleReadback() const { return m_visibleReadback; }
    Resource::GpuResourceHandle GetCullParamsUp() const { return m_cullParamsUp; }
    /// 预创建 CullParams UPLOAD 缓冲（CreateCullingPipeline 主线程调用，避免 Render 线程惰性创建竞态）
    void SetCullParamsUp(const Resource::GpuResourceHandle &handle) { m_cullParamsUp = handle; }
    uint32_t GetAppendUavIndex() const { return m_appendUavIndex; }
    uint32_t GetIndirectArgsUavIndex() const { return m_indirectArgsUavIndex; }
    uint32_t GetAppendCapacity() const { return m_appendCapacity; }
    bool IsCullingResourcesReady() const { return m_appendBuffer.IsValid() && m_indirectArgs.IsValid(); }

private:
    ID3D12Device *m_device = nullptr;
    Resource::DescriptorHeapCollection *m_descHeaps = nullptr;
    Resource::HeapTag m_heapTag = Resource::HeapTag::Default;
    bool m_initialized = false;

    // ── L2b 剔除输出资源 ──
    static constexpr uint32_t kCullDataCapacity = 1 << 20; // CullData 自管 RingBuffer 容量（1MB，~21845 条 CullData）
    Resource::GpuResourceHandle m_cullDataBuffer =
        Resource::GpuResourceHandle::Invalid();         // UPLOAD 自管（CS gCullData 段）
    D3D12_GPU_VIRTUAL_ADDRESS m_cullDataBufferAddr = 0; // 固定地址（CreateCullDataResources 后取，每帧 Map 重写）
    Resource::GpuResourceHandle m_appendBuffer =
        Resource::GpuResourceHandle::Invalid(); // RWStructuredBuffer<uint> 存活实例索引
    uint32_t m_appendCapacity = 0; // AppendBuffer 容量（创建时按实例数分配；超限时重建扩容）
    Resource::GpuResourceHandle m_indirectArgs =
        Resource::GpuResourceHandle::Invalid(); // DRAW_INDEXED_ARGUMENTS×桶数 + 尾部桶偏移表
    Resource::GpuResourceHandle m_bucketOffsetsUp = Resource::GpuResourceHandle::Invalid(); // UPLOAD 桶偏移表前缀和
    Resource::GpuResourceHandle m_bucketArgsUp =
        Resource::GpuResourceHandle::Invalid(); // UPLOAD 每桶 DRAW_INDEXED_ARGUMENTS 静态字段
    Resource::GpuResourceHandle m_zeroUpload =
        Resource::GpuResourceHandle::Invalid(); // UPLOAD 4 字节零（每帧清零各桶 InstanceCount）
    Resource::GpuResourceHandle m_cullParamsUp =
        Resource::GpuResourceHandle::Invalid(); // UPLOAD CullParams（视锥 6 平面 + 实例数）
    Resource::GpuResourceHandle m_visibleReadback =
        Resource::GpuResourceHandle::Invalid();   // READBACK 可见数读回（GPU 写→CPU 读）
    uint32_t m_appendUavIndex = UINT32_MAX;       // Buffer 分区 UAV 槽位（AppendBuffer）
    uint32_t m_indirectArgsUavIndex = UINT32_MAX; // Buffer 分区 UAV 槽位（IndirectArgs）
};

} // namespace Culling
} // namespace DX12Engine
