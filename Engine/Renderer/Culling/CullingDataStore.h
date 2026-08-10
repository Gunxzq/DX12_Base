#pragma once

// ========================================================================
// CullingDataStore — 剔除数据层（L2c 数据扁平化，与 GPU 资源解耦）
//
// 定案（InstanceCullingSystem.md §六）：
//   数据层 = InstanceData/bucketMap/桶偏移表/段地址（数据扁平化与 GPU 解耦）。
//   解除"数据扁平化 ↔ dispatch 生命周期"耦合（2026-08-10 SRV 生命周期断裂根因）。
//
// 迁移（P1，2026-08-10）：从 InstanceCullingBuffer 迁出数据字段/方法——
//   SetFlatInstances 数据部分（赋值 + 桶偏移表前缀和）+ 诊断。旧系统保留作参考。
// ========================================================================

#include "Logger/Logger.h"
#include "Renderer/FrameResources/Struct/FrameResourceTypes.h" // InstanceData（160B——帧管理器仍上传 "Instance" 段）
#include <algorithm>                                           // std::max(initializer_list)
#include <cstdint>
#include <d3d12.h>
#include <vector>

// 剔除数据（CullData，2026-08-10 拆分定案——关注点分离 + 一对多 CullResult 准备）：
// 渲染（输出像素）需要全量 InstanceData（矩阵/材质/探针/顶点偏移）；
// 剔除（Culling）只需精简 CullData（世界位置 + 包围球半径 + 可见性掩码 + 回指索引）。
// 拆分收益：FrameSync 只需复制精简数据（带宽友好）；CS 读取紧凑结构（Cache 友好）；
// 一对多：主相机/CSM 4 级联/反射探针/多光源各一份可见列表，复用同一 CullData（无数据依赖，
// 多 dispatch 并行）——mainViewMask/shadowViewMask 为多视角可见性预留。
// 2026-08-10 迁移：CullData 不再由帧资源管理器上传（剔除层自管 RingBuffer——CullingResourceManager
// m_cullDataBuffer），定义随剔除层归属（此处），不再留在 FrameResourceTypes.h。
namespace DX12Engine {
namespace Renderer {

struct CullData {
    DirectX::XMFLOAT4 worldPos; // 世界坐标 xyz（offset: 0, 16B；w 预留——与 HLSL float4 对齐，避免 float3 stride 错位）
    float boundingRadius;         // 包围球半径（offset: 16, 4B）
    uint32_t mainViewMask;        // 主相机可见性（1 = 可见）
    uint32_t shadowViewMask;      // 预留 16 位给 CSM，16 位给点光/聚光等（多视角剔除结果）
    uint32_t globalInstanceIndex; // 回指到真正渲染用的那个 InstanceData 矩阵索引
    uint32_t bucketOffset;        // 实体桶段起点（CS mapStart——CS 桶遍历分段，随 CullData 上传）
    uint32_t bucketCount;         // 实体桶段长度（CS mapEnd）
    uint32_t pad0;                // 16B 对齐补齐
    uint32_t pad1;                // 16+4+5×4=40B → 48B（与 HLSL float4 stride 16B 完全一致）
};

static_assert(sizeof(CullData) == 48 && sizeof(CullData) % 16 == 0,
              "CullData alignment error: size must be 48B (16B aligned) for HLSL StructuredBuffer compatibility");

} // namespace Renderer
namespace Culling {

class CullingDataStore {
public:
    /// 材质槽桶数上限（与 Shaders/InstanceCulling.cs.hlsl 的 #define kMaxCullBuckets 同步）
    static constexpr uint32_t kMaxCullBuckets = 1024;

    CullingDataStore() = default;
    ~CullingDataStore() = default;

    // ====================================================================
    // 数据扁平化（FrameSync 统一上传阶段——依赖 Builder 分桶结果）
    // ====================================================================

    /// 设置扁平化实例数据（"Instance" 段地址/资源/实例数 + 桶归属表）。
    /// bucketMap = 每实体 [bucketOffset, +bucketCount) 的桶索引扁平表（方案 B 无上限）。
    /// 内部完成桶偏移表前缀和（实体级计数：遍历 m_bucketMap 全表，越界桶归入桶 0）。
    /// @return appendNeed = max(instanceCount, bucketMap.size())——门面据此触发资源层扩容
    uint32_t SetFlatInstances(D3D12_GPU_VIRTUAL_ADDRESS instanceSegmentAddr, ID3D12Resource *instanceRes,
                              uint32_t instanceCount, std::vector<uint32_t> &&bucketMap);

    /// 桶偏移表（前缀和）重新计算并返回——SetFlatInstances 已算，此接口供重算/调试
    const std::vector<uint32_t> &ComputeBucketOffsets();

    // ====================================================================
    // Renderer::CullData（剔除数据，2026-08-10 拆分——上传能力在剔除层）
    // ====================================================================

    /// 设置精简剔除数据（worldPos/boundingRadius/globalInstanceIndex + 桶段），
    /// CS 剔除只读此数组（gCullData，48B 对齐 Cache 友好）；由剔除层上传（原 GPUInstanceData 模式）
    void SetCullData(std::vector<Renderer::CullData> &&cullData);
    const std::vector<Renderer::CullData> &GetCullData() const { return m_cullData; }
    size_t GetCullDataCount() const { return m_cullData.size(); }

    // ====================================================================
    // 数据访问（消费输出——渲染管线/执行层经门面读取）
    // ====================================================================

    uint32_t GetInstanceCount() const { return m_instanceCount; }
    D3D12_GPU_VIRTUAL_ADDRESS GetInstanceSegmentAddr() const { return m_instanceSegmentAddr; }
    ID3D12Resource *GetInstanceRes() const { return m_instanceRes; }
    const std::vector<uint32_t> &GetBucketMap() const { return m_bucketMap; }
    const std::vector<uint32_t> &GetBucketOffsets() const { return m_bucketOffsets; }

    /// 桶 b 在 AppendBuffer 的起始实例偏移（桶偏移表前缀和；越界返回 0）
    uint32_t GetBucketOffset(uint32_t bucketIndex) const {
        if (bucketIndex >= m_bucketOffsets.size())
            return 0;
        return m_bucketOffsets[bucketIndex];
    }
    size_t GetBucketMapSize() const { return m_bucketMap.size(); }

    /// 清空（Shutdown/场景切换）
    void Clear();

private:
    D3D12_GPU_VIRTUAL_ADDRESS m_instanceSegmentAddr = 0; // "Instance" 段 GPU 地址（SetFlatInstances 设置）
    ID3D12Resource *m_instanceRes = nullptr;             // "Instance" 段底层资源（Upload 创建 SRV 用）
    uint32_t m_instanceCount = 0;                        // 实例数（CS SRV NumElements）
    std::vector<uint32_t> m_bucketMap;                   // 方案 B：扁平实体→桶映射表（无上限）
    std::vector<uint32_t> m_bucketOffsets;               // 桶偏移表（前缀和，kMaxCullBuckets+1）
    std::vector<Renderer::CullData> m_cullData;          // 精简剔除数据（CS gCullData，剔除层上传）
};

} // namespace Culling
} // namespace DX12Engine
