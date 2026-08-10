#pragma once

// ========================================================================
// CullingRenderer — 剔除执行层（CS 剔除渲染器，对齐 IRenderer 契约）
//
// 定案（InstanceCullingSystem.md §六）：
//   执行层 = CS dispatch/PSO/根签名/readback 统计（对齐 IRenderer 契约，
//   遵循渲染器模式——规则 24 渲染管线规范）。
//
// 迁移（P3，2026-08-10）：从 InstanceCullingBuffer 迁出 CS 执行部分
//   （CreateCullingPipeline / DispatchCulling / Upload SRV / EndFrame SRV 槽位释放 /
//    CreateComputePipeline / readback）。旧系统保留作参考，稳定后移除。
//
// 依赖：数据层（CullingDataStore：实例数/桶表/段地址）+ 资源层（CullingResourceManager：
// Append/IndirectArgs/CullParams/readback 句柄）——经门面注入引用，执行层不持有资源数据。
// ========================================================================

#include "CullingDataStore.h"
#include "CullingResourceManager.h"
#include "Renderer/FrameResources/FrameResourceManager.h"
#include "Renderer/RHI/Command/CommandList/CommandList.h"
#include "Renderer/RHI/Command/CommandManager.h"
#include "Resource/Core/DescriptorHeapCollection.h"
#include <DirectXMath.h>
#include <cstdint>
#include <d3d12.h>
#include <wrl/client.h>

namespace DX12Engine {
namespace Culling {

class CullingRenderer {
public:
    CullingRenderer() = default;
    ~CullingRenderer() = default;

    // ====================================================================
    // 生命周期（对齐 IRenderer 契约）
    // ====================================================================

    void Initialize(ID3D12Device *device, Resource::DescriptorHeapCollection *descHeaps, Resource::HeapTag heapTag,
                    CullingDataStore *dataStore, CullingResourceManager *resources);
    void Shutdown();

    /// 创建剔除 compute 管线（AppendBuffer/IndirectArgs UAV + compute 根签名/PSO + CullParams）
    /// 惰性创建（首次实例数就绪后，幂等：IsCullingReady() 为 true 后不再创建）
    bool CreateCullingPipeline(Renderer::CommandManager *cmdMgr);

    /// 帧首（立即回调后）：用上一帧 SetFlatInstances 的段创建 gInstances SRV + bucketMap SRV
    bool Upload(Renderer::FrameResourceManager *frameResMgr);

    /// 帧末：释放本帧临时 SRV 槽位（dispatch 已消费；fence 延迟释放；
    /// 段地址不在此清零——生命周期由帧 fence 管理）
    void EndFrame(uint64_t fence);

    // ====================================================================
    // 执行（Render 阶段 PrePass）
    // ====================================================================

    /// 剔除 CS dispatch：视锥球测试 → AppendBuffer/IndirectArgs（gIndirectArgs 每桶 InstanceCount）
    void DispatchCulling(Renderer::CommandList &cmd, const DirectX::XMVECTOR *planes,
                         D3D12_GPU_DESCRIPTOR_HANDLE instanceSRV = {});

    bool IsCullingReady() const; // compute PSO + 资源层 Append/IndirectArgs/CullParams 有效
    bool IsValid() const;        // 段地址 + SRV 槽位有效（经数据层/自身）

    // ====================================================================
    // 消费输出（SRV/地址——渲染管线经门面读取）
    // ====================================================================

    D3D12_GPU_DESCRIPTOR_HANDLE GetInstanceSRV() const;
    D3D12_GPU_DESCRIPTOR_HANDLE GetBucketMapSRV() const;

    // ====================================================================
    // readback / 统计
    // ====================================================================

    uint32_t ReadbackVisibleCount();
    uint32_t ReadbackNonZeroBucketList(uint32_t *outNonZero, uint32_t capacity);
    uint32_t GetLastVisibleCount() const { return m_lastVisibleCount; }
    bool HasDispatched() const { return m_hasDispatched; }
    void SetDiagFrame(uint32_t frame) { m_diagFrame = frame; }

private:
    /// 编译剔除 compute shader + 创建根签名/PSO
    bool CreateComputePipeline();
    /// 在 Buffer 分区创建 StructuredBuffer SRV（指向 RingBuffer 段，Upload 内调用）
    void CreateSRV(ID3D12Resource *ringRes, D3D12_GPU_VIRTUAL_ADDRESS segmentAddr);
    /// 创建 CullData（精简剔除数据，48B）SRV——CS t0 绑定 gCullData（2026-08-10 拆分）
    void CreateCullDataSRV(ID3D12Resource *ringRes, D3D12_GPU_VIRTUAL_ADDRESS segmentAddr);
    /// 创建 EntityBucketMap（uint32 扁平桶归属）SRV（同 RingBuffer 段，stride=4）
    void CreateBucketMapSRV(ID3D12Resource *ringRes, D3D12_GPU_VIRTUAL_ADDRESS segmentAddr);

    ID3D12Device *m_device = nullptr;
    Resource::DescriptorHeapCollection *m_descHeaps = nullptr;
    Resource::HeapTag m_heapTag = Resource::HeapTag::Default;
    bool m_initialized = false;
    bool m_hasDispatched = false; // 2026-08-09：是否已执行过至少一次 dispatch（相机守卫首次兜底用）

    CullingDataStore *m_dataStore = nullptr;       // 数据层引用（门面注入）
    CullingResourceManager *m_resources = nullptr; // 资源层引用（门面注入）

    // ── SRV 槽位（Upload 创建 / EndFrame 释放） ──
    uint32_t m_srvIndex = UINT32_MAX; // Buffer 分区临时 SRV 槽位（CS t0：gCullData 精简剔除数据）
    D3D12_GPU_VIRTUAL_ADDRESS m_cullDataSegmentAddr = 0;  // CullData RingBuffer 段（2026-08-10 拆分）
    D3D12_GPU_VIRTUAL_ADDRESS m_bucketMapSegmentAddr = 0; // bucketMap RingBuffer 段
    uint32_t m_bucketMapSrvIndex = UINT32_MAX;            // bucketMap 临时 SRV 槽位

    // ── PSO / 根签名（执行层自管，对齐渲染器模式） ──
    Microsoft::WRL::ComPtr<ID3D12RootSignature> m_computeRootSig;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_computePSO;

    uint32_t m_lastVisibleCount = 0; // 上次剔除存活数（调试统计）
    uint32_t m_diagFrame = 0;        // 诊断节流帧号（Editor 注入）
};

} // namespace Culling
} // namespace DX12Engine
