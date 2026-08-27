# BugFix：gBucketMeta 布局冲突——CS 非零桶区覆盖桶偏移表（2026-08-16）

> 状态：**已修复，运行时验证正常（用户确认）**
> 现象：相机完全不动但三帧渲染画面完全不同（持续性错乱/闪烁）——实体与阴影均受影响
> 定位手段：RenderDoc 抓帧 + 三帧资源内容导出对比（决定性证据）
> 关联：`BugFix_L2c_InstanceCulling_ContentCorruption.md`（L2c 内容损坏排查链，2026-08-09）、
> `ShadowFix_Snapshot_20260815.md`（阴影修正，InstanceData 布局统一/偏移策略对齐）、
> `BlockBaking.md` §七·五（每桶独立缓冲 Nanite 改造）

## 一、现象

- **相机完全静止（输入确定）但每帧渲染画面不同**（持续错乱/闪烁，非偶发）
- 实体绘制与阴影绘制均受影响
- 发生在每桶独立缓冲（Nanite 改造）引入后

## 二、RenderDoc 三帧观察（2026-08-16）

抓取相机不动连续三帧，导出以下数据对比：

| 导出 | 文件 | 帧间差异 | 结论 |
|:--|:--|:--|:--|
| 录制命令 | `录制命令-1/2/3.txt` | **结构完全一致**（2 Dispatch + N ExecuteIndirect + CopyBufferRegion 序列） | 命令序列非差异源 |
| PSO 状态 | `pso状态-1/2/3.html` | 一致 | 非差异源 |
| **每桶 InstanceCount** | `InstanceCulling_BucketIndirectArgs[1]-1/2.csv` | **完全一致**（diff 空） | ✅ CS 每桶写入稳定 |
| **CullData（CS 输入）** | `CullData-1/2.csv` | **完全一致**（diff 0） | ✅ 剔除输入确定（排除跨帧覆盖） |
| **BucketMeta（桶偏移表区）** | `InstanceCulling_BucketMeta-1/2.csv` | **112 行差异** | ❌ 桶偏移表被覆盖 |
| **Append（gAppend）** | `InstanceCulling_Append-1/2.csv` | **1582 行差异** | ❌ VS 读实例索引错乱 |

**决定性证据**：`InstanceCulling_BucketMeta-1.csv` 前 30 行：
```
0, 59  1, 57  2, 46  3, 6  4, 7  5, 8  6, 42  7, 4  8, 54  9, 47...
```
这些值是**非零桶编号列表**（59/57/46/6/7/8/42/4/54/47 是桶号），**不是桶偏移表前缀和**（前缀和应递增 0,5,12,20...）——CS 把非零桶列表写进了桶偏移表区域。

## 二·五、确定性结论（排他性，避免后续重复排查）

相机完全不动（输入确定）下三帧对比，**确定性排除**以下假设（已由数据证明，后续排查无需再验证）：

| 排除项 | 确定性证据 | 结论 |
|:--|:--|:--|
| **CullData 跨帧覆盖** | CullData-1/2.csv **逐字节一致**（diff 0 行） | ✅ **CullData 不存在跨帧异常**——CS 剔除输入确定且稳定；CullData RingBuffer 化/bucketOffsetsUp 8 帧放大**不是本次根因**（与 ShadowFlicker 2026-08-13 回退记录一致） |
| **CS 每桶写入问题** | BucketIndirectArgs[1]-1/2.csv **完全一致**（diff 空） | ✅ CS 每桶 InstanceCount 写入稳定正确——排除 CS 剔除逻辑/每桶 UAV 绑定问题 |
| **命令序列差异** | 录制命令-1/2/3.txt 结构一致 | ✅ 排除 dispatch/ExecuteIndirect/Copy 序列变化 |
| **PSO/状态差异** | pso状态-1/2/3.html 一致 | ✅ 排除 PSO 切换/状态问题 |

**锁定范围**：唯一帧间差异 = **BucketMeta（桶偏移表区，112 行差异）+ Append（gAppend，1582 行差异）**——桶偏移表被污染（非零桶列表覆盖）→ gAppend 段基址错 → 错乱源锁定在 **gBucketMeta 布局**。

## 三、根因：gBucketMeta 布局冲突

**CS 非零桶区索引错误**（`InstanceCulling.cs.hlsl` AppendToBucket）——写入 CPU 桶偏移表区域：

```hlsl
// 错误（修复前）：非零桶区写 [0]/[1..]，覆盖 CPU 桶偏移表 [0..1024]
InterlockedAdd(gBucketMeta[0], 1u, listIdx);   // 非零桶计数 → gBucketMeta[0]
gBucketMeta[1u + listIdx] = bucket;            // 非零桶列表 → gBucketMeta[1..]

// CPU 布局（DispatchCulling）：
//   gBucketMeta[0..kMaxCullBuckets] = 桶偏移表（前缀和，COPY 自 bucketOffsetsUp）
//   gBucketMeta[(kMaxCullBuckets+1)..] = 非零桶区（计数+列表）
```

**错乱机制**：
```
CS 写非零桶计数/列表到 [0..] → 覆盖桶偏移表（[0..1024] 被桶编号填充）
CS 读 gBucketMeta[bucket]（桶偏移表）→ 拿到非零桶编号（59/57/46...）而非前缀和
→ gAppend 段基址错（各桶存活索引写错位置，互相覆盖）
→ VS gAppend[gBucketBase + instanceID] 读错实例索引 → 世界矩阵错
→ 持续性错乱（闪烁）
```

## 四、修复（2026-08-16）

`InstanceCulling.cs.hlsl` **AppendToBucket**（262-263 行）非零桶区索引对齐 CPU 布局：

```hlsl
// 修复后：非零桶区在桶偏移表之后
InterlockedAdd(gBucketMeta[kMaxCullBuckets + 1u], 1u, listIdx);   // 非零桶计数 → [1025]
gBucketMeta[(kMaxCullBuckets + 1u) + 1u + listIdx] = bucket;      // 非零桶列表 → [1026..]
```

**ShadowAppendToBucket 无需修复**：阴影精简路径无非零桶区写入（注释"无 HZB/非零桶区/readback"），只读 `gBucketMeta[bucket]` 桶偏移表。

## 五、验证

- **运行时已正常**（用户确认 2026-08-16）——持续错乱（闪烁）消除
- 静态验证：CS 非零桶区索引（[kMaxCullBuckets+1] 计数 / [kMaxCullBuckets+2..] 列表）↔ DispatchCulling 清零偏移（(kMaxCullBuckets+1)）对齐；无残留旧索引 `gBucketMeta[0]`
- 复核手段：RenderDoc 重新导出 BucketMeta——前部应为**递增前缀和**（非桶编号列表），相机不动两帧一致

## 六、排查经验（避免重复）

1. **持续错乱 vs 偶发错乱是两码事**：持续 = 每帧复现的确定性错误（布局/索引/状态），偶发 = 低频时序/残留；排查手段不同
2. **相机不动抓三帧**：输入确定时，命令结构一致 + 资源内容不一致 → 差异锁定在 CS 写入/资源内容（RenderDoc 导出资源 CSV 逐项 diff）
3. **优先级**：每桶 InstanceCount（CS 输出稳定）+ CullData（CS 输入稳定）→ 排除 CS 写入与跨帧覆盖 → 剩余 = 桶偏移表（BucketMeta）→ 布局冲突
4. **旧模式阴影修复参考**（ShadowFix_Snapshot_20260815.md）：根因 = 结构体不匹配 + 偏移策略不匹配——本次同类（gBucketMeta 布局不匹配 CS 索引）
