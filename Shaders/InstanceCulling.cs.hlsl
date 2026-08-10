// ============================================================================
// InstanceCulling.cs.hlsl — L2b GPU 实例剔除（GPU-Drive.md §三/§五 阶段 4）
//
// 输入：
//   t0: StructuredBuffer<GPUInstanceData> gInstances（L2a 静态一次上传）
//        实例数据：row_major float4x4 world（平移在 _41.._43）+ float radius + uint bucketOffset + uint bucketCount
//   t1: StructuredBuffer<uint> gBucketMap（方案 B 扁平实体→桶映射表，长度 = Σ 实体材质段桶；
//       每实体 [bucketOffset, bucketOffset+bucketCount) 为其材质段桶列表，无固定上限）
//   b0: CullParams（视锥 6 平面 + 实例总数）
// 输出：
//   u0: RWStructuredBuffer<uint> gAppend（存活实例索引，Append 语义手写 InterlockedAdd，按桶分段）
//   u1: RWStructuredBuffer<uint> gIndirectArgs（D3D12_DRAW_INDEXED_ARGUMENTS 5 字段 × kMaxCullBuckets，
//       每桶 [1] = InstanceCount 由原子计数递增；尾部 kMaxCullBuckets+1 个 uint = 桶偏移表前缀和，
//       CPU 每帧 COPY 写入，CS 读取作为 gAppend 分段基址）
//
// 每实例一个 thread：包围球（center + radius）vs 视锥 6 平面（保守不误删——
// 球心到平面距离 < -radius 才判外，与 CPU FrustumCullSphere 同语义）
// ============================================================================

// L2c 材质槽桶数上限（与 C++ InstanceCullingBuffer::kMaxCullBuckets 同步，超出归入桶 0；
// 2026-08-08：64 → 1024，City 场景 Builder 批次 ~250 个，64 过小致超限批次聚合桶 0 绘制错乱）
#define kMaxCullBuckets 1024

// 单桶最大子网格段数（与 C++ RenderItemCommon::kMaxSubMeshRanges 同步，定案 7.2a：
// 同材质多段聚合一桶，ExecuteIndirect MaxCommandCount=段数 一次提交；CS 对每段 InstanceCount 同步递增）
#define kMaxSubMeshRanges 8

// 非零桶区起始地址（空桶跳过 2026-08-09，与 C++ indirectArgUints 布局同步）：
// gIndirectArgs 前部 = kMaxCullBuckets×kMaxSubMeshRanges×5 uint（DRAW_INDEXED_ARGUMENTS 段区），
// 尾部 = 桶偏移表 (kMaxCullBuckets+1) uint，再后 = 非零桶区：
//   [kNonZeroCountAddr]                        = 非零桶计数
//   [kNonZeroCountAddr+1 .. +kMaxCullBuckets]  = 非零桶索引列表
//   [kNonZeroTotalAddr]                        = 总可见实例数（存活实例总数，Verify 用）
#define kNonZeroCountAddr (kMaxCullBuckets * 5u * kMaxSubMeshRanges + (kMaxCullBuckets + 1u))
#define kNonZeroTotalAddr (kNonZeroCountAddr + kMaxCullBuckets + 1u)

// 方案 B（2026-08-08，见 GPU-Drive.md §五 / L2c_Todo.md §六）：实例 = 实体（1 实体 = 1 包围球 = 1 剔除票）。
// bucketIndices[4]（固定上限 4，5 槽实体第 5 桶截断 → 空桶错乱）废弃——
// CullData 拆分（2026-08-10 定案，关注点分离 + 一对多 CullResult 准备）：
//   CS 剔除只读精简 CullData（worldPos/boundingRadius + 桶段），与全量 InstanceData（矩阵/材质/探针）
//   分离——FrameSync 只需复制精简 CullData（带宽友好），CS 读取紧凑结构（Cache 友好）。
//   mainViewMask/shadowViewMask 为多视角可见性预留（主相机/CSM/反射探针/多光源各一份 CullResult，
//   复用同一 CullData，无数据依赖，多 dispatch 并行）；globalInstanceIndex 回指 InstanceData 矩阵索引。
//   CullData 上传能力在剔除层（CullingDataStore），对齐原 GPUInstanceData 模式。
struct CullData
{
    float4 worldPos;          // 世界坐标 xyz（0-15B，w 预留——float4 与 C++ XMFLOAT4 对齐，避免 float3 stride 错位）
    float boundingRadius;     // 包围球半径（16-19B）
    uint mainViewMask;        // 主相机可见性（1 = 可见；预留多视角写回）
    uint shadowViewMask;      // 预留 16 位给 CSM，16 位给点光/聚光等（多视角剔除结果）
    uint globalInstanceIndex; // 回指到真正渲染用的 InstanceData 矩阵索引
    uint bucketOffset;        // 实体桶段起点（CS mapStart）
    uint bucketCount;         // 实体桶段长度（CS mapEnd）
    uint pad0;                // 16B 对齐补齐（40B → 48B，与 C++ 完全一致）
    uint pad1;
};

StructuredBuffer<CullData> gCullData : register(t0);
StructuredBuffer<uint> gBucketMap : register(t1);
RWStructuredBuffer<uint> gAppend : register(u0);
RWStructuredBuffer<uint> gIndirectArgs : register(u1);

cbuffer CullParams : register(b0)
{
    float4 gPlanes[6]; // 视锥 6 平面（A,B,C,D；平面法线指向内部）
    uint gInstanceCount;
    uint gPad0;
    uint gPad1;
    uint gPad2;
};

[numthreads(64, 1, 1)] void CSMain(uint3 dtid : SV_DispatchThreadID)
{
    if (dtid.x >= gInstanceCount)
        return;

    CullData inst = gCullData[dtid.x];
    // 拆分后：视锥测试用 CullData 精简字段（worldPos.xyz/boundingRadius）——不再读全量矩阵
    float3 center = inst.worldPos.xyz;
    float radius = inst.boundingRadius;

    // 视锥测试（保守：球心到平面距离 < -expandedRadius 才剔除）
    // 与 CPU FrustumCullSphere（CullingUtil.h）同语义：radius × 1.15 扩展保护带，
    // 否则视锥边缘实例在相机微动时反复可见/不可见 → 频闪 + 意外剔除
    bool inside = true;
    const float expandedRadius = radius * 1.15f;
    for (int i = 0; i < 6; ++i)
    {
        float d = dot(gPlanes[i].xyz, center) + gPlanes[i].w;
        if (d < -expandedRadius)
        {
            inside = false;
            break;
        }
    }
    if (!inside)
        return;

    // 存活：对实体拥有的每个材质段桶分别计数（定案 7.2a：实例=实体，1 包围球 1 剔除票——
    // 实体可见后其每个材质段桶都 +1 并写实体索引）。
    // gIndirectArgs 前部 = 每桶 kMaxSubMeshRanges 段 × D3D12_DRAW_INDEXED_ARGUMENTS（5 uint），
    // 每段 [1] 为 InstanceCount（同桶各段计数相同：同一实例集合画多个索引段）；
    // 尾部 = 桶偏移表前缀和（CPU 每帧 COPY），桶 b 的 Append 段基址 = gIndirectArgs[kMaxCullBuckets*5*kMaxSubMeshRanges + b]；
    // 尾部再后 = 非零桶区（空桶跳过 2026-08-09）：[0]=非零桶计数，[1..kMaxCullBuckets]=非零桶索引列表——
    // 每桶首个可见实例（InterlockedAdd 返回旧值 0）InterlockedIncrement 计数并写列表。
    // 方案 B：桶归属来自扁平映射表 gBucketMap（每实体 [bucketOffset, bucketOffset+bucketCount)），
    // 无 kMaxBucketsPerEntity 固定上限——不再截断多材质实体（truncatedRefs 修复）
    const uint mapStart = inst.bucketOffset;   // CullData：桶段起点（原 meta.y）
    const uint bucketCount = inst.bucketCount; // CullData：桶段长度（原 meta.z）
    for (uint k = 0u; k < bucketCount; ++k)
    {
        // 防御：映射表越界读（bucketOffset/bucketCount 写错）时归入桶 0
        uint bucket = 0u;
        uint mapIdx = mapStart + k;
        if (mapIdx < 0xFFFFu)
        { // gBucketMap 越界会读到垃圾——用紧凑上限兜底（实际由 CPU 保证长度）
            uint b = gBucketMap[mapIdx];
            bucket = (b < kMaxCullBuckets) ? b : 0u;
        }
        // 桶 b 的段区基址（每桶 kMaxSubMeshRanges 段 × 5 uint）
        uint segBase = bucket * 5u * kMaxSubMeshRanges;
        // 段 0 InstanceCount 递增并写 Append（Append 每桶一段，写一次实体索引）
        uint slot;
        InterlockedAdd(gIndirectArgs[segBase + 1], 1, slot);
        // 非零桶记录：本桶首个实例（旧值 slot==0）→ 计数 +1 并写入列表（供 CPU 跳过空桶）
        // 注意：HLSL 无 InterlockedIncrement——递增用 InterlockedAdd(dest, 1, orig)
        if (slot == 0u)
        {
            uint listIdx;
            InterlockedAdd(gIndirectArgs[kNonZeroCountAddr], 1u, listIdx); // 返回旧值 = 列表写入下标
            gIndirectArgs[kNonZeroCountAddr + 1u + listIdx] = bucket;
        }
        // 防御性上限（2026-08-09）：slot = 本桶 InstanceCount 旧值（InterlockedAdd 返回）——
        // 正常受 CPU 桶偏移表约束（前缀和，slot < 桶实例数）。极端情况下（桶偏移表与
        // 实际计数跨帧错位）slot 可能超 gAppend 容量 → 写越界 → 内存损坏/错乱。
        // 上限保护：slot 超过 64K 防御值则跳过写入（宁可少画不可写坏内存）。
        if (slot < 0x10000u)
        {
            gAppend[gIndirectArgs[kMaxCullBuckets * 5u * kMaxSubMeshRanges + bucket] + slot] = inst.globalInstanceIndex;
            // total 统计：每写一个存活实例递增（Verify 读回可见总数）——InterlockedAdd(dest,1)
            uint totalOrig;
            InterlockedAdd(gIndirectArgs[kNonZeroTotalAddr], 1u, totalOrig);
        }
        // 其余段 InstanceCount 同步递增到相同值（ExecuteIndirect MaxCommandCount=段数 读各段 InstanceCount）
        for (uint s = 1u; s < kMaxSubMeshRanges; ++s)
        {
            uint dummy;
            InterlockedAdd(gIndirectArgs[segBase + s * 5u + 1], 1, dummy);
        }
    }
}
