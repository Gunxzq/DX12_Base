#pragma once

// ========================================================================
// CullingLayer — 剔除层门面（Facade）
//
// 定案（InstanceCullingSystem.md §6.1，2026-08-10）：
//   剔除层 = 门面——包装剔除系统模块（SpatialHashGrid / CullingDataStore /
//   CullingResourceManager / CullingRenderer）及其方法，对外统一暴露。
//   渲染管线（构建器/渲染器）只与剔除层交互，不感知内部模块。
//
// 实装（P0，2026-08-10）：门面转发到三层模块；Editor/Builder 调用点改走门面，
// 旧系统（InstanceCullingBuffer/OctreeSystem）保留作参考，稳定后移除。
// ========================================================================

#include "CullingDataStore.h"
#include "CullingRenderer.h"
#include "CullingResourceManager.h"
#include "ECS/World.h"
#include "Renderer/Core/CulledSet.h"
#include "Renderer/FrameResources/FrameResourceManager.h"
#include "Renderer/Scene/Struct/Frustum.h"
#include "Scheduler/FrameDriver.h"
#include "SpatialHashGrid.h"
#include <cstdint>
#include <d3d12.h>
#include <memory>
#include <vector>

namespace DX12Engine {
namespace Culling {

class CullingLayer {
public:
    /// 拆分期单例（对齐旧 InstanceCullingBuffer::GetInstance 模式；门面聚合后
    /// Editor 调用点改走此处，旧系统保留作参考，稳定后移除单例改显式注入）
    static CullingLayer &GetInstance();

    CullingLayer() = default;
    ~CullingLayer() = default;

    CullingLayer(const CullingLayer &) = delete;
    CullingLayer &operator=(const CullingLayer &) = delete;

    // ====================================================================
    // 初始化 / 销毁
    // ====================================================================

    void Initialize(ID3D12Device *device, Resource::DescriptorHeapCollection *descHeaps,
                    Resource::HeapTag heapTag = Resource::HeapTag::Default);
    void Shutdown();

    // ====================================================================
    // 视锥输入（每帧帧首注入——输入方式 B：剔除系统有视锥体就够）
    // ====================================================================

    /// 注入最新预测视锥（相机预测不归剔除系统，TAA 等多处使用，留在相机系统）
    void SetPredictedFrustum(const Renderer::Frustum &frustum);
    const Renderer::Frustum &GetPredictedFrustum() const { return m_predictedFrustum; }

    // ====================================================================
    // ECS 数据连接（方案 C：注入 World，内部 GetRegistry 遍历）
    // ====================================================================

    /// 绑定 ECS World（内部空间索引遍历）；多 World/多场景 → sceneId 映射
    void BindWorld(ECS::World &world);

    // ====================================================================
    // system 注册（剔除层自注册；暴露唯一 task name 供构建器 DependsOn）
    // ====================================================================

    /// 注册全部剔除 task（PreCulling 空间粗筛 + Render 阶段 CS dispatch）
    void RegisterSystems(Scheduler::FrameDriver &driver);
    static const char *GetTaskNameSpatialQuery(); // "CullingSystem/OctreeQuery"
    static const char *GetTaskNameCSDispatch();   // "CullingSystem/CSDispatch"

    // ====================================================================
    // 上传阶段（时序契约：上传先于消费——立即回调 Upload → 本帧 dispatch 消费）
    // ====================================================================

    /// 帧首（立即回调后）：用上一帧 SetFlatInstances 的段创建 SRV
    void Upload(Renderer::FrameResourceManager *frameResMgr);
    /// 帧末（FrameSync）：数据扁平化（供下一帧 Upload）
    /// 实例数据扁平化 + 容量守卫（数据层 SetFlatInstances → 资源层 ResizeAppendBuffer 扩容）
    /// @param releaseFence 扩容时旧 AppendBuffer 的延迟释放 fence（Editor 传 GetNextFence——
    /// 2026-08-10 #921 修复：传 0 立即释放旧缓冲而 GPU dispatch 仍 in-flight → OBJECT_DELETED_WHILE_STILL_IN_USE）
    void SetFlatInstances(D3D12_GPU_VIRTUAL_ADDRESS instanceSegmentAddr, ID3D12Resource *instanceRes,
                          uint32_t instanceCount, std::vector<uint32_t> &&bucketMap, uint64_t releaseFence);
    /// 帧末：释放 SRV 槽位（段地址保留，帧 fence 管理）
    void EndFrame(uint64_t fence);

    // ====================================================================
    // 消费输出（中间层聚合——渲染管线只经此读取，不感知内部）
    // ====================================================================

    const Renderer::CulledSet &GetCoarseSet() const; // L1 粗筛候选（Builder 分桶输入）
    /// 粗筛结果写入（Editor OctreeCullingSystem 查询/合并完成后注入；GetCoarseSet 读取）
    void SetCoarseSet(const Renderer::CulledSet &set) { m_coarseSet = set; }
    /// Renderer::CullData 写入（Editor FrameSync 生成精简剔除数据；CullingRenderer::Upload 上传为 CS gCullData）
    void SetCullData(std::vector<Renderer::CullData> &&cullData) { m_dataStore.SetCullData(std::move(cullData)); }
    D3D12_GPU_DESCRIPTOR_HANDLE GetInstanceSRV() const;       // gInstances（"Instance" 段）
    D3D12_GPU_DESCRIPTOR_HANDLE GetBucketMapSRV() const;      // gBucketMap（扁平桶归属）
    D3D12_GPU_VIRTUAL_ADDRESS GetBufferAddress() const;       // 实例段地址
    const std::vector<uint32_t> &GetBucketOffsets() const;    // 桶偏移表（前缀和）
    uint32_t GetInstanceCount() const;                        // 实例数（CS SRV NumElements）
    uint32_t GetBucketOffset(uint32_t bucketIndex) const;     // 桶 b 起始实例偏移
    size_t GetBucketMapSize() const;                          // 扁平桶归属表大小
    uint32_t GetLastVisibleCount() const;                     // 上次剔除存活数（调试统计）
    D3D12_GPU_VIRTUAL_ADDRESS GetAppendBufferAddress() const; // AppendBuffer（存活实例索引）GPU 地址
    ID3D12Resource *GetIndirectArgsResource() const;          // IndirectArgs 资源指针（ExecuteIndirect 消费）
    void SetBucketDrawArgs(
        uint32_t bucketIndex, uint32_t subMeshCount,
        const Renderer::RenderItemCommon::SubMeshRange *ranges = nullptr); // 预置桶 DRAW_INDEXED_ARGUMENTS

    // ====================================================================
    // 剔除执行（Render 阶段 PrePass——EditorInstanceCullingSystem 转发目标）
    // ====================================================================

    bool CreateCullingPipeline(Renderer::CommandManager *cmdMgr);
    void DispatchCulling(Renderer::CommandList &cmd, const DirectX::XMVECTOR *planes,
                         D3D12_GPU_DESCRIPTOR_HANDLE instanceSRV = {});
    bool IsCullingReady() const;
    bool IsValid() const;
    bool HasDispatched() const { return m_renderer.HasDispatched(); }
    uint32_t ReadbackVisibleCount() { return m_renderer.ReadbackVisibleCount(); }
    uint32_t ReadbackNonZeroBucketList(uint32_t *outNonZero, uint32_t capacity) {
        return m_renderer.ReadbackNonZeroBucketList(outNonZero, capacity);
    }
    void SetDiagFrame(uint32_t frame) { m_renderer.SetDiagFrame(frame); }

    // ====================================================================
    // 模块访问（门面转发——拆分期供内部/调试，外部调用点保持不变）
    // ====================================================================

    SpatialHashGrid &SpatialIndex() { return m_spatialIndex; }
    CullingDataStore &DataStore() { return m_dataStore; }
    CullingResourceManager &Resources() { return m_resources; }
    CullingRenderer &Renderer() { return m_renderer; }

private:
    ECS::World *m_world = nullptr;        // ECS 数据源（BindWorld 注入）
    Renderer::Frustum m_predictedFrustum; // 视锥输入（SetPredictedFrustum 注入）
    SpatialHashGrid m_spatialIndex;       // 模块 1：空间粗筛（粗筛层）
    CullingDataStore m_dataStore;         // 模块 2：数据层（共享引用经门面）
    CullingResourceManager m_resources;   // 模块 3：资源层（生命周期集中管理）
    CullingRenderer m_renderer;           // 模块 4：执行层（CS dispatch/PSO）
    Renderer::CulledSet m_coarseSet;      // L1 粗筛结果缓存（Editor 查询后注入，GetCoarseSet 读取）
    bool m_initialized = false;
};

} // namespace Culling
} // namespace DX12Engine
