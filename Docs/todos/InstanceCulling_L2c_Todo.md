# InstanceCulling L2c 落地与相关待办

> 日期：2026-08-08
> 背景：GpuResourceHandle 无效值修复（0xFFFFFFFF）后，L2b GPU 剔除 dispatch 已真实执行
> （visible 随相机实时波动，City4 46464 实例 48-57 FPS），但**剔除结果尚未被绘制链路消费**。
> 关联：`Docs/snapshots/InstanceCulling_MemoryGrowth_TDR_Snapshot_20260808.md` §六、
> `Docs/bugs/BugFix_InstanceCulling_CullParamsCBV_HandleInvalid.md`、
> `Docs/architecture/rendering/GPU-Drive.md`（L2 剔除设计）、`.atomcode.md` 规则 24/25

---

## 一、待办清单

| # | 任务 | 优先级 | 说明 |
|:-:|:-----|:-------|:------|
| 1 | **L2c 间接绘制落地（GPU 剔除结果驱动绘制）** | **P0** | `AppendBuffer`/`IndirectArgs` 已由 L2b dispatch 生成但**零消费**——`GetAppendBufferAddress`/`GetIndirectArgsAddress` 定义后无调用，`DrawIndexedInstancedIndirect` 仅存在于注释（Editor.cpp:1183）。落地后砍掉 CPU 精筛（Builder 每帧 queue=130~254 全量精筛），最高收益 |
| 2 | **场景切换实体残留 → 实例数虚增** | P1 | `CloseTab`（EditorSceneManager.cpp:1749）注释明确"实体已在 Registry 中，不清除"——场景关闭只存快照不销毁实体。City4 二次构建（12:59:55）实例收集 46464 = 15488×3（新旧实体各收集一次），块数 6→12。修复：按 `SceneTagComponent.sceneId` 销毁场景实体（或复用既有实体） |
| 3 | SRV 每帧重建 | P2 | Upload 每帧 Allocate/Free 临时 SRV 槽位 + `CreateShaderResourceView`（816-891 次/帧，日志 `L2a SRV created`）。实例数据静态时可一次性 SRV + 每帧仅上传矩阵增量 |
| 4 | 实例数据全量上传 | P2 | 46464×80B ≈ 3.7MB/帧全量 `Allocate("InstanceCulling", ...)`。静态实例可分块按需上传/复用 |
| 5 | readback 节流 | P3 | `m_visibleReadback` 每帧 CopyBufferRegion + 120 帧读回，仅统计用途（代码注释明确"此缓冲仅统计"）。可降频或仅在调试启用 |

---

## 二、L2c 落地的预期改造点（P0 展开）

```
现状：L2b dispatch → AppendBuffer(存活索引) + IndirectArgs(InstanceCount) → 无人消费
目标：L2b 输出 → DrawIndexedInstancedIndirect 直接驱动绘制

核心架构前提（用户定案，2026-08-08）：
- 数据扁平化：所有实例合并为"全局实例缓冲"，实例携带 materialIndex/bucketIndex
- 逻辑扁平化：单 CS 一遍过所有类型实体（现有 InstanceCulling.cs.hlsl 已是该形态——每实例
  一个 thread 做包围球视锥剔除），输出可见索引列表
- 绘制扁平化：CPU 不再遍历实体，各桶发 DrawIndexedInstancedIndirect 消费 GPU 可见集
```

### 架构澄清（用户定案，2026-08-08）：Builder 桶 ≠ 剔除桶，无冲突，仅规划错位

| 层 | 职责 | 语义 |
|:--|:--|:--|
| Builder 桶 | **材质槽维度**分发——把绘制渲染项（OpaqueRenderItem）分发给不同渲染器（Opaque/Water/Transparent 等），各有固定运行阶段与顺序 | 桶 = 材质槽（material slot），为材质槽服务 |
| 剔除桶（L2c） | GPU 可见性分段——CS 剔除后按桶计数，供间接绘制消费 | bucketIndex = 材质槽索引（与 Builder 桶对齐） |

**两者不冲突**：Builder 桶决定"哪个渲染器/阶段绘制"，剔除桶决定"该材质槽下哪些实例可见"。数据扁平化在 **FrameSync 统一上传阶段**（`FrameSync_EditorUploadInstanceData`，Editor.cpp:1140-1178）完成——Builder 已产出材质槽分桶结果，统一上传时把实例矩阵 + bucketIndex（材质槽索引）扁平化写入 GPUInstanceData，L2b CS 消费。

### 定案修正（2026-08-08 后续）：实例=实体，桶=材质段（含子网格聚合）

**问题**：当前实现把"子网格"当成了"实例"——同一实体的每个材质段（子网格）独立成桶，world 矩阵复制 N 份、CS 剔除同一包围球 N 次，ExecuteIndirect 数 = 子网格数级（实测 1800 条，每桶 InstanceCount 仅 1~8）。

**定案（对照大型引擎：UE GPUScene 实例级剔除 + Unity BRG materialID 批次）**：

| 层 | 语义 | 粒度 |
|:--|:--|:--|
| **实例（剔除票）** | 1 实体 = 1 包围球 = 1 次剔除，**不按子网格拆分**（mesh 内部多材质段不影响剔除） | 实体级 |
| **桶（绘制命令）** | 按材质段聚合：同材质的多子网格区间并入一桶（`ExecuteIndirect MaxCommandCount=段数` 一次提交多段）；材质切换才分桶 | 材质段级（= 材质槽） |
| **CS 计数** | 实体可见后对"其拥有的每个材质段"分别原子计数（分段 Append） | 材质段 × 可见实体 |

**关键认知**：材质切换必须分桶（无法一个 Indirect 画两种材质）——目标不是"1 条命令"，而是 **命令数 = 材质段数、剔除票 = 实体数**。这是 UE GPUScene（实例级剔除）与 Unity BRG（materialID 批次）的标准范式。

### 设计方案（对齐用户"全局可见性计算管道"建议）

| 步骤 | 改动 | 文件 |
|:--|:--|:--|
| ① GPUInstanceData 扩展 | `pad[3]` 改 `uint bucketIndex; float2 pad;`（**保持 80B**）——bucketIndex = 材质槽索引 | `InstanceCullingBuffer.h`、`InstanceCulling.cs.hlsl` ✅ 已落地 |
| ② 桶表（BucketTable） | **材质槽桶表**：Builder 按材质槽分桶，FrameSync 统一上传时把 `bucketIndex`（材质槽紧凑索引）回填到实例数据；桶表存 {材质槽 → 渲染器/阶段、baseInstanceOffset} | `FrameSync_EditorUploadInstanceData`（Editor.cpp:1140）+ `InstanceCullingBuffer` |
| ③ IndirectArgs 扩容 | 从 5 uint（单桶）→ `桶数×5` uint，每桶一段（D3D12_DRAW_INDEXED_ARGUMENTS）；CS 改为 `InterlockedAdd(gIndirectArgs[bucketIndex*5+1], 1, slot)` | `CreateUAVs` |
| ④ Append 分段 | `gAppend` 按桶分段（桶偏移表）或全局 Append + 桶区间映射（先全局 + 桶 offset 表，最小改动） | `InstanceCulling.cs.hlsl` |
| ⑤ VS 变体 | Opaque VS：`inst = gInstanceData[gAppend[SV_InstanceID]]`（间接索引）替代 `gInstanceData[SV_InstanceID]` | `color.hlsl`/GBuffer VS |
| ⑥ OpaqueRenderer 间接路径 | 新增 `DrawIndexedInstancedIndirect(cmd, bucketIndirectArgsAddress, ...)`；保留现有 `DrawInstancedGBuffer` 作为回退 | `OpaqueRenderer.h/.cpp` |
| ⑦ EditorOpaqueRenderSystem | 剔除就绪时走间接绘制（每桶一次 Indirect），`IsCullingReady()` 为 false 时回退 CPU 精筛队列 | `Editor.cpp` |

### 验证路径（单块 → 多块）

1. **单块单桶验证**：City 场景相机固定 → 对比 `DrawIndexedInstanced(instanceCount)` 与 `DrawIndexedInstancedIndirect` 输出一致性（visible 计数 = IndirectArgs.InstanceCount）
2. **多桶**：Builder 分桶数 > 1 时验证每桶 InstanceCount 独立计数正确
3. **回退验证**：`IsCullingReady()` false / 剔除失败 → 自动回退 CPU 精筛（现状）
4. 相机静止缓存（GPU-Drive.md 阶段 5b）：ViewProj 未变跳过 compute，复用 IndirectArgs

### 数据层已落地（2026-08-08，单桶兼容）

| 改动 | 文件 | 状态 |
|:--|:--|:--|
| `GPUInstanceData` 加 `bucketIndex`（80B 不变：world64+radius4+bucket4+pad8） | `InstanceCullingBuffer.h` | ✅ |
| HLSL 结构对齐 + CS 按桶计数 `gIndirectArgs[1 + bucketIndex*5]`（bucketIndex=0 等价原全局计数） | `InstanceCulling.cs.hlsl` | ✅ |
| readback 偏移 4B = 桶 0 InstanceCount（单桶兼容） | `InstanceCullingBuffer.cpp` | ✅ 无需改 |
| bucketIndex 回填（材质槽紧凑索引） | **FrameSync 统一上传**（Editor.cpp:1140-1178）由 Builder 分桶结果写出，非 CollectFromBlocks | 待多桶 |

### 日志确认点（数据层就绪验证）

| 日志 | 位置 | 预期 |
|:--|:--|:--|
| `[L2c] bucket 分布`（Info，节流 120 帧） | FrameSync 统一上传尾部（Editor.cpp:1140 回调内） | 单桶阶段 `buckets=1 instances=6853`；多桶落地后显示材质槽桶数 |
| `[L2c] IndirectArgs 桶计数`（Info，节流 120 帧） | dispatch 后 readback | 单桶阶段 `bucket0=visible` 与 `[Verify]` 一致；多桶后每桶独立 |
| `[Verify] visible/total`（已有） | Editor.cpp:1213 | 保持——确认剔除链路持续健康 |

### 风险与约束

- **扁平化与分发解耦**：Builder 桶（材质槽 → 渲染器/阶段）只做分发，剔除桶（bucketIndex → GPU 可见性分段）只做计数——两者通过 FrameSync 统一上传回填的 bucketIndex 对齐，无需合并数据结构
- SRV 段偏移语义（RingBuffer 单段连续性）：Append 的全局索引必须映射到上传矩阵的段内偏移
- 动态物体/建筑状态切换不进 L2（GPU-Drive.md 关键权衡）

---

## 三、事实依据（2026-08-08 日志）

| 项 | 证据 |
|:--|:--|
| L2b dispatch 真实执行 | `[Verify] visible=5833→1294→5072→1559`（City）/ `9033→3018→2586`（City4 46464），随相机实时波动 |
| L2c 未消费 | `GetAppendBufferAddress`/`GetIndirectArgsAddress` 仅头文件定义（InstanceCullingBuffer.h:118/125），无调用点；`DrawIndexedInstancedIndirect`/`ExecuteIndirect` 无实现 |
| City4 实例虚增 | switchId=1: `15488 instances in 6 blocks`；switchId=2: `46464 instances in 12 blocks`（=15488×3，CloseTab 未清实体） |
| 帧率基线 | City 6853 实例 ~50 FPS；City4 15488 ~38.6；City4 46464 ~28（剔除开销 culling=0.01ms 可忽略，瓶颈在绘制负载） |

---

## 四、后续步骤

1. **P0（L2c 落地）**：评估 IndirectArgs/AppendBuffer 绑定到 Opaque 绘制的最小改动（先单块验证 → 多块）；确认 SRV 段偏移语义（RingBuffer 单段连续性）在间接绘制下的约束
2. **P1（实体残留）**：场景关闭时按 sceneId 销毁实体；修复后复测 City4 实例数（应回落 15488）与帧率
3. 人工编译验证（项目规则 AI 不编译）

---

## 五、异常调查：RenderDoc ExecuteIndirect 63→2233 vs Perf queue=242（2026-08-08 晚）

### 现象

RenderDoc 捕获显示 **ExecuteIndirect 事件区间 63→2233（≈2170 次）**，远超 Perf 日志 `queue=242`（Opaque 渲染项数 = 每渲染项一次 ExecuteIndirect）。用户观察每条 `ExecuteIndirect(maxCount 1, count <1>)` 内部仅一条命令。

### 已排除的根因（数据证据）

| 假设 | 判定 | 证据 |
|:--|:--|:--|
| 子网格级拆分（1800 桶回归） | ❌ 排除 | JSON 统计 **81 个 (geometry×材质) 去重组合**（6853 mesh 实体 / 44 geometry：mapChip×5、buildingHigh×3 等），与 2170 差 27 倍 |
| 材质段级拆分未生效 | ❌ 排除 | SceneConstructor 聚合生效（`tree slot#0: subMeshRange` ×2）、Builder `{geometry,materialIdx}` 分桶、buckets=170 材质段级 |
| ExecuteIndirect 内部多段展开 | ❌ 排除 | RenderDoc 显示 `maxCount=1, count=1`（每桶一段） |
| AppendBuffer 容量越界 | ❌ 排除（本轮已修复） | 17:25 运行扩容警告 0 次；此前 17:08 运行 `Verify total=615 < flat instances=861` 曾触发修复（m_appendCapacity + ResizeAppendBuffer） |

### 当前判定（待 RenderDoc 过滤确认）

- **大概率：2170 = RenderDoc 全帧 draw call 统计**（ExecuteIndirect 242 + Lighting/SSAO/Water/Wireframe/PostProcess/UI/Preview 等其他 pass 累计 ≈1900 条），非 L2c 路径膨胀
- 确认方法：RenderDoc **过滤只看 `ExecuteIndirect` 类型命令**——若 ≈242 则 L2c 无缺陷；若仍 ~2000 则 Builder 实际生成 2000+ 渲染项，需继续深挖 pendingBatches 计数
- 用户观察"2300 是实体渲染器的部分"——需区分 RenderDoc 统计口径（全帧 vs 单 pass）

### 数据层性能基线（已证实正常）

`flat instances: 971 (96B/instance), buckets=170`；`queue=242`；`[Verify] visible=852~960 / total=860~968 (88~111%)`；builder=0.10-0.15ms；ExecuteIndirect 242 次（材质段级）

---

## 六、方案 B：桶归属移出实体结构（2026-08-08 定案，逐步推进中）

### 6.1 问题记录（已坐实）

**现象**：City 场景 `[FrameSync][Diag] truncatedRefs=253` 恒定——253 个 5 槽实体（mapChip06 等）的**第 5 个材质段桶引用被截断**。

**证据链**（2026-08-08 日志 + RenderDoc）：
- `[FrameSync][Diag] batches=196 skipped=0 instTotal=2273 flatInstances=939 bucketsUsed=196 truncatedRefs=253`（truncatedRefs 恒 = 场景 5 槽实体数 253）
- `[InstanceCullingBuffer][Diag] bucketOffsets: usedBuckets=127~135` ≪ `bucketsUsed=196~207`（~60-70 桶无实例引用）
- RenderDoc：`IndirectDrawIndexed(<N, 0>)` 空桶 15 条（被截断桶 InstanceCount 恒 0）
- 效果：被截断桶对应材质段不绘制 → 运行时错乱

**根因**：`GPUInstanceData.bucketIndices[4]` + `kMaxBucketsPerEntity=4`（C++ InstanceCullingBuffer.h + HLSL InstanceCulling.cs.hlsl 双端硬编码）假设单实体材质段桶数 ≤4。但材质桶数 = 场景 JSON `materials[]` 长度（资产数据，任意），**无约束保证 ≤4**。固定数组承载不确定长度的桶归属，属设计缺陷（用户定案：不能假定材质桶数量）。

### 6.2 方案 B 设计

| 设计点 | 内容 |
|:--|:--|
| 原则 | **无固定上限**：实体材质段桶数 = Σ 实体槽位（动态），对齐 UE GPUScene material 分段思想 |
| `GPUInstanceData` 新布局 | 只保留 `world/radius/bucketOffset/bucketCount`（96B→约 80B 对齐），移除 `bucketIndices[4]` |
| `EntityBucketMap`（新缓冲） | 扁平 `{uint32 bucketIdx}[]`（按实体分段连续存储），长度 = Σ 实体槽位，GPU SRV |
| CS 读取 | 实体可见后 `for (m = bucketOffset; m < bucketOffset + bucketCount; ++m) 对 gBucketMap[m] 桶计数`——替代固定数组遍历 |
| FrameSync 生成 | 构建扁平映射表：实体首遇写 `bucketOffset = map.size()`，每新桶 `map.push_back(bucketIdx)`、`bucketCount++` |
| 兼容 | 剔除仍按实体（1 票）；桶偏移表/Append 分段/ExecuteIndirect 消费不变 |

#### 6.2a 字节级设计定案（2026-08-08，保持 96B stride 最小改动）

**关键决策**：新 `GPUInstanceData` **保持 96B/实例**（SRV stride、RingBuffer 分配大小、对齐均不变——只替换 `bucketIndices[4]`(16B) 为 `bucketOffset`(4B) + 扩容 pad）。

```cpp
// C++（InstanceCullingBuffer.h）——与 HLSL 布局一致
struct GPUInstanceData {
    DirectX::XMFLOAT4X4 world; // offset 0, 64B（行主序）
    float radius;              // offset 64, 4B
    uint32_t bucketOffset;     // offset 68, 4B —— EntityBucketMap 内本实体桶列表起始下标
    uint32_t bucketCount;      // offset 72, 4B —— 本实体材质段桶数（无上限，不再截断）
    float pad[5];              // offset 76, 20B —— 补到 96B（16 对齐：76+20=96）
};
// 删除 kMaxBucketsPerEntity=4 常量（不再有单实体桶数上限）
```

```hlsl
// HLSL（InstanceCulling.cs.hlsl）——与 C++ 一致
struct GPUInstanceData {
    row_major float4x4 world; // 64B
    float radius;
    uint bucketOffset;        // gBucketMap 起始下标
    uint bucketCount;         // 桶数（无上限）
    float4 pad[1];            // 16B → 总 96B（64+4+4+4+16 = 92？不对——用 float5 pad 后 HLSL 对齐）
};
```

**对齐修正（HLSL 16 字节对齐陷阱）**：HLSL 结构体成员按 16B 对齐——`float radius`(offset 64) + `uint bucketOffset`(68) + `uint bucketCount`(72) + `float pad[5]`(76~96)。但 HLSL 中 `float pad[5]` 数组元素按 16B 对齐会膨胀。**等价安全写法**：

```hlsl
struct GPUInstanceData {
    row_major float4x4 world;  // 0..63
    float radius;              // 64
    uint bucketOffset;         // 68
    uint bucketCount;          // 72
    float pad[2];              // 76..83（HLSL 数组对齐到 16B 边界，实际占用到 96？）
    float pad2;                // 显式补尾
};
// 实测后以 sizeof 对齐为准；C++ 96B 与 HLSL 必须逐字节一致（RenderDoc Mesh Viewer 可验证）
```

**`EntityBucketMap` 缓冲**：
- 类型：`StructuredBuffer<uint>`（每元素 = 1 个 bucketIdx，4B）
- 长度：动态 = Σ(实体 bucketCount)（City 场景实测 ~9505 槽位级，无固定上限）
- 存储：FrameResourceManager 新 RingBuffer 段（"InstanceCullingBucketMap"，随实例数据同帧上传，Reclaim 自动回收）
- SRV：临时槽位（同 InstanceSRV 模式，EndFrame 释放）；CS 根签名新增 t1 槽位

**CS 读取**（替代 `bucketIndices[]` 固定数组 + `min(inst.bucketCount, 4u)` 截断）：
```hlsl
StructuredBuffer<uint> gBucketMap : register(t1);
...
if (!inside) return;
// 无上限遍历：bucketCount 由 FrameSync 精确写入，gBucketMap 长度 = Σ bucketCount
for (uint m = inst.bucketOffset; m < inst.bucketOffset + inst.bucketCount; ++m) {
    uint bucket = gBucketMap[m];
    uint segBase = bucket * 5u * kMaxSubMeshRanges;
    uint slot;
    InterlockedAdd(gIndirectArgs[segBase + 1], 1, slot);
    gAppend[gIndirectArgs[kMaxCullBuckets * 5u * kMaxSubMeshRanges + bucket] + slot] = dtid.x;
    for (uint s = 1u; s < kMaxSubMeshRanges; ++s)
        InterlockedAdd(gIndirectArgs[segBase + s * 5u + 1], 1, dummy);
}
```

**FrameSync 生成**（Editor.cpp 回调内，替代 entityIndex 去重内的 bucketIndices 追加）：
```cpp
std::vector<uint32_t> bucketMap; // 扁平映射表
bucketMap.reserve(8192);
// 实体首遇：
g.bucketOffset = static_cast<uint32_t>(bucketMap.size());
g.bucketCount = 1;
bucketMap.push_back(assignedBucket);
// 实体已存在且新桶（不再截断）：
if (!dup) { g.bucketCount++; bucketMap.push_back(assignedBucket); }
// 尾部：上传 bucketMap → InstanceCullingBucketMap RingBuffer + SRV
```

**容量/防御**：`bucketMap.size()` 动态增长（无上限）；`bucketIdx >= kMaxCullBuckets` 仍归入桶 0（与 CS 越界防御一致，仅防桶索引溢出，不再限制实体桶数）。

### 6.3 实施步骤（逐步推进）

1. **C++ 侧**：`GPUInstanceData` 重构（去 bucketIndices[4]，加 bucketOffset/bucketCount）+ `EntityBucketMap` 缓冲创建（SRV，长度 = Σ 槽位，按需扩容）/上传
2. **CS 着色器**：新增 `gBucketMap`（StructuredBuffer<uint>），实体可见后按 `bucketOffset..+bucketCount` 遍历计数，替代 `bucketIndices[]` 固定数组 + `min(inst.bucketCount, 4u)` 截断
3. **FrameSync**：生成扁平映射表 `vector<uint32> bucketMap` + 写回每实体 `bucketOffset/bucketCount`（替代当前 entityIndex 去重内的 bucketIndices 追加）
4. **容量**：`EntityBucketMap` 长度 = 动态 Σ 槽位（无上限；超 kMaxCullBuckets 的桶索引仍归入桶 0 防御）
5. **验证**：truncatedRefs 消失、usedBuckets ≈ bucketsUsed、空桶消失、多槽实体全材质段正常绘制；人工编译（项目规则 AI 不编译）

### 6.4 关联

- 设计记录：`Docs/architecture/rendering/GPU-Drive.md` §五 阶段 5（已知缺陷 + 方案 B 定案）
- 本文 §一 待办 #1（L2c 落地）为前置；方案 B 不改变桶/渲染项/ExecuteIndirect 结构，仅改"实体→桶"归属表达

## 七、2026-08-09 空桶跳过 / 合并路径 / 错乱根因记录

### 7.1 CS 编译错误导致剔除停摆（已修复，决定性）

- **现象**：渲染全错乱（相机静止也错乱），日志 `Compile InstanceCulling.cs.hlsl failed: 0x80004005` + `L2b compute pipeline creation failed`
- **根因**：非零桶记录用了 `InterlockedIncrement`——**HLSL 无此标识符**（错误 X3004），原子递增必须用 `InterlockedAdd(dest, 1, orig)`。编译失败 → compute PSO 未创建 → `IsCullingReady()` false → 守卫每帧拦截 → dispatch 从未执行 → gIndirectArgs 是创建时未初始化的垃圾 InstanceCount → ExecuteIndirect 读垃圾
- **修复**：2 处 `InterlockedIncrement` → `InterlockedAdd(dest, 1u, orig)`（非零桶计数 + total 统计）
- **教训**：HLSL 原子函数名与 C++ 语义不同（无 InterlockedIncrement），且 CS 编译失败会让整个剔除链路静默停摆——日志中的 PSO 创建失败是关键信号

### 7.2 空桶跳过三度落地（带场景未变化保护）

- **目标**：跳过 InstanceCount=0 的空桶 ExecuteIndirect（CPU 省 SetPSO+ExecuteIndirect 调用；GPU 免空桶命令解析）
- **数据流**：CS 每桶首个可见实例（`InterlockedAdd` 返回旧值 0）写非零桶列表（gIndirectArgs 尾部非零桶区：计数+列表+total）→ readback 单次 COPY（替代逐桶 1024 次 CopyBufferRegion）→ Editor 相机静止帧读回列表跳过空桶
- **三次回退记录**：
  1. 首次：readback 未初始化（垃圾计数）→ 误跳 → 回退
  2. 二次：CS 编译错误（7.1）→ dispatch 从未执行 → readback 垃圾 → 回退
  3. 三次：readback 列表在场景变化时过期（相机静止 dispatch 被跳过 → 保留旧列表；异步加载/实体增删后桶结构更新 → 旧列表误跳本应绘制的桶 → 漏画错乱）→ 回退
- **最终方案（三度落地）**：`IsReadbackFresh()`——桶结构版本戳（`m_currentStamp = m_bucketMap.size()` 于 SetFlatInstances；`m_lastDispatchStamp` 于 DispatchCulling 成功处置位）。仅当 `!m_cameraMoved && culling.IsReadbackFresh()` 时启用跳过；异步加载期间（版本戳变化）或相机移动帧 → 全提交
- **命令导出验证**：空桶跳过生效时 ExecuteIndirect 从 249 → 142（-43%），空桶 117 → 0

### 7.3 bindless 桶键（{geometry, shaderType}）回退——段区聚合破坏材质一致性

- **尝试**：BatchKey 去 materialIdx（`{geometry, materialIdx}` → `{geometry, shaderType}`），材质降为实例级（PS 从 `gInstanceData[InstanceIndex].MaterialIndex` 取）
- **错乱根因**：同 geometry 不同材质实体并入一桶 → 桶内 subMeshRanges 聚合多个材质的子网格区间 → ExecuteIndirect 每段 InstanceCount 相同（全部实例画全部段）→ 实例被画到不属于自己材质的子网格段 → 材质错乱（日志 multiSegBuckets 0→64 铁证）
- **本质**：ExecuteIndirect 无法按实例区分段（每段 InstanceCount 相同），材质只能随桶（段区=材质子网格一致性）。**桶必须保留材质维度**
- **修正认知**：批次 = mesh × material 是大型引擎标准（Unity BRG / UE 均如此），跨材质合并不是标准路径

### 7.4 方案 A（StartInstance 直索引）二度回退——StartInstanceLocation 不可靠

- **尝试**：桶偏移从每桶 CBV（gBucketBase）改为 `DRAW_INDEXED_ARGUMENTS.StartInstanceLocation`（ExecuteIndirect 合并后唯一逐段机制），VS 直索引 `gAppendBuffer[SV_InstanceID]`
- **错乱根因**：两次实测 SV_InstanceID 未从 bucketBase 起（StartInstanceLocation 生效依赖驱动）→ 直索引读错桶实例 → 渲染散乱
- **修复**：StartInstance 恒 0 + VS 恢复 `gAppendBuffer[gBucketBase + instanceID]`（每桶 CBV 路径）
- **合并结论**：ExecuteIndirect 多段合并受"逐段区分桶偏移"的根本限制（StartInstanceLocation 依赖驱动），除非在可靠驱动验证，否则合并收益无法落地

### 7.5 错乱根因：PS 材质 bindless 回退（多槽实体实例级 MaterialIndex 是首次遇见快照）

- **现象**：回退 bindless 桶键 + 方案 A 后仍错乱（日志确认运行 City.scene 6854 实体，非 City4）
- **根因**：PS 材质 bindless（实例级 `gInstanceData[InstanceIndex].MaterialIndex`）——实体级实例数据是"首次遇见快照"（FrameSync 扁平化），多槽实体（KD03 8 槽）8 个桶共用同一实例 → 实例级 MaterialIndex 只有首槽材质 → 8 槽全部用首槽材质错乱
- **修复**：PS 恢复 `(gL2cEnabled != 0) ? gBucketMaterialIndex : gInstanceData[...]`——L2c 用每桶 CBV（桶={geometry, materialIdx}，桶内实例同材质，正确）；回退路径仍用实例数据
- **结论**：材质索引必须桶级（gBucketMaterialIndex），不能实例级——除非实例=实体×材质槽展开（未来方向）

### 7.6 大型引擎调研（Unity BRG / UE GPUScene）

- **Unity BRG**：`BatchDrawCommandIndirect` 每个 draw command 引用 materialID + meshID（材质批次级），批次粒度 = mesh × material，每批次独立 indirect draw；`visibleOffset` 仅批次内实例定位，不用于跨批次合并
- **UE**：`FMeshDrawCommand::MatchesForDynamicInstancing` 仅合并"完全相同 shader bindings"（同 PSO 同材质同参数）；材质参数在 material uniform buffer（per-draw 绑定）；GPUScene = primitive 数据全局缓冲（实例数据 bindless，shader 用 PrimitiveID 索引）
- **结论**：我们当前架构（桶={geometry, materialIdx} + 每桶一次 ExecuteIndirect + 实例数据 bindless）**已是大型引擎标准形态**；跨材质合并不是标准路径（UE 仅合并完全相同绑定），合并方向应修正

### 7.7 当前状态与下一步

- **安全基线**：BatchKey={geometry, materialIdx} + StartInstance 恒 0 + VS gBucketBase 路径 + PS gBucketMaterialIndex + 空桶跳过（IsReadbackFresh 保护）+ readback 单次 COPY + HasDispatched 首次兜底
- **待办**：人工编译验证（项目规则 AI 不编译）；确认渲染恢复后按 §7.2/7.4 结论固化
- **下一步优化空间**：见 §8

## 八、下一步优化空间（2026-08-09 评估）

### 8.1 GPU 侧空桶跳过（推荐，优先级高）

- **现状**：空桶跳过为 CPU 侧（readback 列表 + 相机静止 + IsReadbackFresh 保护），收益已验证（ExecuteIndirect 249→142，-43%）；但依赖 readback 延迟 1 帧 + 场景未变化条件，相机移动帧不生效
- **方向**：CS 已写非零桶计数（gIndirectArgs 尾部 kNonZeroCountAddr）——用 **ExecuteIndirect 的 countBuffer 参数**让 GPU 直接只执行非零桶命令：`ExecuteIndirect(sig, maxCount, args, offset, countBuffer, countBufferOffset)` 读 countBuffer 值作为命令数（min(count, maxCount)），空桶 count=0 → GPU 跳过 0 条命令
- **前置**：CS 需写"每桶命令数"（非零桶=段数，空桶=0），或将非零桶列表压缩为连续命令区
- **收益**：无 CPU 读回延迟、无场景变化条件、相机移动帧也生效；对齐大型引擎 GPU-driven 方向
- **风险**：countBuffer 语义是"命令条数"非"实例数"，需确认与 DRAW_INDEXED 签名交互（每桶段数不同时命令数≠桶数）

### 8.2 实例=实体×材质槽展开（解锁 bindless 材质，中优先级）

- **现状**：PS 材质必须桶级（gBucketMaterialIndex）——实体级实例是"首次遇见快照"，多槽实体（KD03 8 槽）实例级 MaterialIndex 只有首槽材质 → 8 槽共用首槽材质错乱（§7.5）
- **方向**：实例按 (实体, 材质槽) 展开（1 实体 N 槽 = N 条实例，各带自己 MaterialIndex）→ 材质可实例级 bindless → 桶键可去 materialIdx（前提：同 geometry 实体子网格区间一致，City4 满足）
- **代价**：实例数膨胀（Σ 槽位 vs 实体数），剔除票 ×槽位；需评估 City4（全单槽，无膨胀）与多槽场景（KD03）的权衡
- **收益**：桶数降为几何体数（City4 157→几何数），ExecuteIndirect 命令数大降

### 8.2a C 方案详细设计（2026-08-09 用户推进：实体 bindless，几何不解体）

**用户目标**：完整立方体（一个几何）用两种纹理完整绘制，不拆子网格——材质差异经 bindless 在着色器内处理，几何整体为一个绘制单元。

**现状核查（三个关键事实）**：
1. 顶点格式无材质索引字段（TriangleMesh 无 per-vertex MATERIAL）——"完整不解体 + 每面材质"需顶点扩展，属三角形级（Nanite）方向，非当前
2. FrameSync 实体级聚合（Editor.cpp L1201/L1222 `a.inst = inst` 首次遇见快照）——多槽实体实例级 MaterialIndex 只有首槽（§7.5 根因）
3. Builder 已每槽 push 实例（OpaqueRenderItemBuilder.cpp L260-261 `instData.MaterialIndex = materialIdx`）——展开数据已具备，缺 FrameSync 去重放宽

**实现路径（三端）**：
1. **FrameSync**：实体级去重 → (实体, 材质槽) 展开——`a.inst` 快照改为每槽位一条（MaterialIndex = 槽位材质），实例数据天然携带正确材质
2. **CS/剔除**：剔除票语义从"实体"改"实体×槽位"（多槽实体 N 票）——或保留实体级剔除、展开仅影响实例数据（需评估 gAppend 写实体索引的槽位区分）
3. **PS/Builder**：材质可实例级（PS `gInstanceData[InstanceIndex].MaterialIndex` 恢复 bindless）；桶键评估去 materialIdx

**风险**：实例数膨胀（KD03 8 槽 × 实体数）；剔除票语义变化；ExecuteIndirect 段区与材质槽对应（§7.3 段区聚合教训——展开后每槽实例画自己的子网格区间，需确保段区=槽位子网格）

**优先级**：City4（全单槽）先行验证（无膨胀），KD03 多槽场景作为回归——完整实现需用户拍板剔除语义 + 人工编译验证，当前记录待实施

### 8.3 合并方向修正（对齐 UE，低优先级）

- **现状**：方案 A（StartInstance 直索引）两次实测 SV_InstanceID 未从 bucketBase 起（StartInstanceLocation 依赖驱动）→ 合并受阻（§7.4）
- **方向**：不再追求跨材质合并——大型引擎（Unity BRG / UE）批次 = mesh × material，不跨材质合并（§7.6）；UE 仅合并"完全相同 shader bindings"
- **可行**：若未来驱动验证 StartInstanceLocation 可靠，可重试方案 A（同 PSO 桶段区打包一次 ExecuteIndirect）；当前不做

### 8.4 readback 节流/移除（低优先级）

- **现状**：readback 单次 COPY 每帧执行（验证用途 + 空桶跳过数据源）
- **方向**：空桶跳过 GPU 化（§8.1）后，readback 仅剩验证用途 → 恢复 120 帧节流或按需读回
- **收益**：省每帧 1 次 COPY + barrier

### 8.5 City4 场景压力测试（验证收益放大）

- City4（15489 实体、~250 桶）为 PSO 桶合并/空桶跳过的完美测试场景（99.99% shaderType=0）
- 验证空桶跳过（§7.2 保护）+ 帧率基线，量化优化收益

### 8.6 优先级建议

1. **§8.1 GPU 侧空桶跳过**（收益大、无延迟限制、对齐大型引擎）
2. **§8.5 City4 压力测试**（验证当前基线收益）
3. **§8.2 实例展开 bindless**（架构演进，需多槽场景权衡）
4. **§8.3/8.4**（低优先/待驱动验证）

## 九、2026-08-09 静态低帧率 / 相机静止守卫排查记录

> 用户主线问题："静态相机帧率比动态还低，以前从未有过"。逐层排查后确认：**dispatch 相机静止守卫多余（已移除），Octree 查询守卫是大型引擎标准（已恢复）**。RenderDoc 证实 GPU 剔除/ExecuteIndirect 命令链路正常。

### 9.1 dispatch 相机静止守卫移除（用户决策，收益≈0）

- **问题**：`EditorInstanceCullingSystem` 相机静止时跳过 dispatch（复用 gIndirectArgs）——`dispatchCalls=1 / systemCalls=275`（120 帧内仅 1 次 dispatch）
- **代价**：gIndirectArgs / gAppend / readback 三态过期（场景异步加载/实例数据每帧变化时与当前 gInstanceData 脱节 → 偶发索引错位错乱）
- **收益**：仅省 0.01ms（日志 culling=0.01ms 实证）——跳过省不了什么
- **大型引擎对照**（§7.6 调研深化）：UE / Unity BRG / DOOM / Far Cry 5 的 **GPU 细剔除（dispatch）每帧执行**；缓存的是 **CPU 粗筛可见集**（UE PrimitiveVisibility 位图）——"相机不动就跳过 GPU 剔除"非业界做法
- **决策**：移除守卫（Editor.cpp L1390 仅留 `IsCullingReady()`），每帧 dispatch（gIndirectArgs/gAppend 与 gInstanceData 始终同帧）

### 9.2 cameraMoved 检测修复：ViewProj 矩阵 → Position/Forward（浮点误差误判）

- **问题**：相机静止但 cameraMoved=true 持续（日志 28 true / 17 false）→ 每帧 Clear + 重建 coarse → 渲染项每帧变化 → 偶发错乱 + 帧率波动
- **根因**：`CameraManager::CalculateMatrices` 每帧重算（Normalize/Cross/矩阵乘法浮点误差）→ ViewProjMatrix 每帧微小差异 → 旧检测 `Σ|ΔViewProj| > 1e-3` 误判移动
- **修复**：改为 Position + Forward 精确比较（相机静止时这两个值精确不变）——`cameraMoved = |ΔPos| > 1e-3 || |ΔForward| > 1e-3`
- **验证**：`[CameraManager][Diag] cam: vel=(0,0,0)`（静止 Velocity 恒 0，用户判断"Velocity 线性无残留"正确）+ 修复后静止 cameraMoved=false

### 9.3 Octree 查询相机静止守卫恢复（大型引擎标准）

- **背景**：9.1 移除的是 dispatch 守卫；Octree 查询守卫（EditorCullChunk/EditorCullMerge 的 `!m_cameraMoved` 跳过）一度被一并移除
- **后果**：移除后静止也每帧查询 → coarse 反映真实视锥覆盖（coarse=104/25 大片）→ 渲染项剧增（queue 264~296）→ 静止帧率极低（3~8FPS，用户实测"以前从未有过"）
- **对照**：Octree 查询守卫 = CPU 粗筛静止缓存（UE PrimitiveVisibility 同思路）——**合理**，与 dispatch 守卫（GPU 剔除）性质不同
- **修复**：恢复 EditorCullChunk（L978）+ EditorCullMerge（L1004）的 `!m_cameraMoved` 守卫（9.2 修复后检测精确，恢复安全）：静止复用 coarse（渲染少帧率正常）、运动时重建（与当前视锥一致）

### 9.4 AppendBuffer 预分配 ×2（运行时扩容 GPU 同步阻塞）

- **问题**：日志 `AppendBuffer capacity overflow` 2 次（29776 needed > 27420）→ ResizeAppendBuffer 重建（Release 旧资源 + 重建 UAV + 屏障）→ GPU 同步阻塞 → 帧率骤降
- **根因**：初始容量按当前值分配（`max(实例数, Σ桶引用)`），场景加载中 bucketMap 增长（异步加载）→ 触发运行时扩容
- **修复**：初始 `appendNeed × 2` 余量（InstanceCullingBuffer.cpp L567-568），覆盖加载期增长；大场景仍需 ResizeAppendBuffer（其内同样预留）

### 9.5 静态帧率异常排查链（RenderDoc 证实命令链路正常）

- **日志证据**：315.4ms/3FPS（coarse=104 queue=264 builder=0.14ms culling=0.01ms）；readback=0（0桶）
- **RenderDoc 双帧录制**（Frame #352/#355，静态命令录制1/2.txt）：
  - 264 次 ExecuteIndirect，InstanceCount 分布两帧**完全一致**（84×0、52×1、28×3、20×12、16×34，最大 34，无垃圾值）
  - **GPU 剔除 + ExecuteIndirect 命令链路完全正常**（gIndirectArgs 有效）
  - 差异仅普通绘制（72 vs 81 DrawIndexedInstanced，透明/水路径）
- **结论**：315ms 帧为旧版本行为（dispatch 守卫拦截 → gIndirectArgs 未更新/readback=0 的帧）；当前源码（守卫移除 + 每帧 dispatch）编译后应恢复正常

### 9.6 当前状态与结论

- **已修复**：dispatch 守卫移除（9.1）、cameraMoved Position/Forward 检测（9.2）、Octree 查询守卫恢复（9.3）、AppendBuffer 预分配 ×2（9.4）
- **待编译验证**：全部改动未编译（用户确认"附加收尾根本没编译运行过"，日志为旧版本）——编译后确认 `dispatchCalls≈systemCalls`（每帧 dispatch）+ 静止帧率恢复
- **架构定案**：CPU 粗筛（Octree）可静止缓存（守卫保留）；GPU 细剔除（dispatch）每帧执行（守卫移除）——对齐大型引擎
- **诊断日志保留**：`[CameraManager][Diag] cam:`（Velocity）、`[OctreeCullingSystem][Diag] entered`（cells/cameraMoved）、`[CullDiag] chunk queried`、`[InstanceCulling][Diag] systemCalls/dispatchCalls`

## 十、绘制错乱排查状态（2026-08-09 晚，未定案）

> 现象：偶发绘制错乱——**部分内容位置正确、部分错误，错误内容叠加到可见范围**；几何不变形、世界位置画错；线框模式下仍可见。详见 `bugs/BugFix_L2c_InstanceCulling_ContentCorruption.md`。

### 10.1 已排除（全链路验证）

```
✅ 块粒度（缩小块仍偶发）            ✅ 块级命中（非画不画）
✅ GPU 流程（GBV 无警告）            ✅ dispatch 守卫（已移除）
✅ 预测相机差距（factor 2.0 未加剧）  ✅ 并行拼接 / subMeshRange 边界
✅ 桶偏移帧间错位（同帧号对比一致）    ✅ 原子竞争（InterlockedAdd 硬件原子）
✅ 两段 RingBuffer 错位（[SegDiag]） ✅ flatInstances/allInstances 同序
✅ 首遇快照取错批次（[FirstSnap]）    ✅ 实例材质突变（[EntDiag] 本次恒 27）
⚠️ PS 桶材质（[BucketMat] 样本少未覆盖错乱帧）
```

### 10.2 剩余疑点（下一步方向）

```
A. 样本盲区：错乱帧未在抽样（节流 60~120 帧 + 抽样桶 0/64/128 未覆盖）
B. GPU 内读取：VS flatInstanceIndex / gAppend 内容（静态正确需运行时证据）
C. 桶归属变化瞬间的帧间错位（低频偶发）

验证手段：
  1. 错乱发生时立即看 [BucketMat]/[EntDiag]/[L2cOffset] 同帧数据
  2. PIX GPU 捕获（抓 VS 实际读取的 flatInstanceIndex/World）——决定性
```

### 10.3 本轮新增诊断日志（保留）

| 日志 | 观察层 | 位置 |
|:--|:--|:--|
| `[EntDiag]` | 实体 name/World/材质/桶段（固定目标每帧） | Editor.cpp FrameSync |
| `[FirstSnap]` | 多桶实体首遇批次材质 | Editor.cpp accum 首遇快照 |
| `[L2cOffset]` | VS 用 gBucketBase | Editor.cpp 提交循环 |
| `[CullOffset]` | CS 用偏移表（同帧号） | InstanceCullingBuffer.cpp |
| `[SegDiag]` | 两段 RingBuffer 地址 | Editor.cpp FrameSync |
| `[BucketMat]` | PS 用 gBucketMaterialIndex | Editor.cpp CullParamsL2c |
| `[OpaqueDraw]` | 每桶 VB/IB/实例段地址 | OpaqueRenderer.cpp |

### 10.4 快照关联

- GPUDriven_Snapshot_20260806.md（剔除分层基线）
- InstanceCulling_MemoryGrowth_TDR_Snapshot_20260808.md（RingBuffer 内存/TDR）
- 本轮错乱排查全记录见 bugs/BugFix_L2c_InstanceCulling_ContentCorruption.md

### 10.5 旧状态异常推测（bindless 推进时处理）

```
静态验证已达极限（数据/时序/生命周期/同步全部正常）——错乱根因未能在数据层捕获。
用户推测（未确认）：可能是【旧状态导致的异常】——
  gIndirectArgs/gAppend/gInstanceData 某些段在特定帧保留上一场景/上一帧
  旧状态（未清零/未覆盖），ExecuteIndirect 在帧间同步窗口读到旧数据。

处理计划（bindless 推进时）：
  1. GPUInstanceData 段布局重设计（bindless 统一实例数据）
  2. 显式清零/版本戳（gIndirectArgs InstanceCount 每帧显式归零 +
     段代际标记，杜绝跨帧旧状态读取）
  3. 届时可验证/消除旧状态异常假设

防御性代码已下放（2026-08-09 定案）：
  - Builder radius 下限保护（max(radius, 1e-3)，防 CS 球测试失效）
  - CS gAppend 写入 slot 上限保护（< 0x10000，防写越界）
  - GetBucketOffset 越界返回 0（已有）

### 10.6 单实例缓冲合并（2026-08-09 已实施）

```
动机：双实例段（"Instance" InstanceData 96B + "InstanceCulling" GPUInstanceData 96B）
  是成功案例（UE GPU Scene/Unity BRG/MS 示例）中不存在的形态——两份独立上传，
  帧间同步窗口可能读到不同代际 → 移动闪烁（用户观察）+ 旧状态异常（§10.5 推测）。
  合并为单一实例缓冲（语义：单一事实源/统一索引/生命周期/帧原子性）。

实施（2026-08-09）：
  1. InstanceData 扩展 160B：+bucketOffset/bucketCount/pad0/pad1（剔除 meta 并入）
     （FrameResourceTypes.h）
  2. FrameSync 扁平化：meta 写入 InstanceData（不再生成 GPUInstanceData/flatInstances），
     单次 Allocate 上传 "Instance" 段（Editor.cpp）
  3. SetFlatInstances 签名改：接收 (segmentAddr, instanceRes, instanceCount, bucketMap)
     ——CS 绑定 "Instance" 段（不再独立上传实例段）（InstanceCullingBuffer）
  4. Upload 改造：用 m_instanceRes 创建 CS gInstances SRV（跳过实例段分配）
  5. CS（InstanceCulling.cs.hlsl）：GPUInstanceData 结构改 InstanceData 镜像 160B，
     读取改直接字段（radius/bucketOffset/bucketCount——原 meta.x/y/z）
  6. VS（color.hlsl）：InstanceData 结构对齐 160B（补 radius/bucketOffset/bucketCount/pad）
  7. CreateSRV stride/firstElement 改 sizeof(InstanceData)=160B（对齐陷阱修复）

关键陷阱（用户提醒）：C++ InstanceData 160B 与 CS/VS 的 HLSL 结构必须严格一致
  （stride 错位 → CS 读 gInstances 错位 → 剔除全灭）——已核对 160B = 160B。

预期：合并后 VS/CS 从同一 "Instance" 段读同一份数据（帧原子）——
  移动闪烁（双段不同步）与旧状态异常（§10.5）从语义上消除。

## 十一、单实例缓冲合并后阻塞排查（2026-08-09 晚，未闭环）

### 11.1 状态：第三步（CS 用 InstanceData 段）后【长期阻塞】——核心问题未定位

```
现象：第三步编译运行后长期阻塞（画面空/无渲染，主线程卡死感知）
排查进展：
  ✅ 卡死点 1（已修）：Upload 时序——Upload 在 SetFlatInstances 之前执行
     → 首次帧段未设置 → SRV 未创建 → dispatch skipReady（已改为 SetFlatInstances 后）
  ✅ 卡死点 2（已修）：Allocate("Instance") 未按 160B 对齐 → CreateSRV firstElement 错位
     → CS 读错 → GPU 挂起（已加 alignment=sizeof(InstanceData)）
  ✅ flatInstances（GPUInstanceData）已移除（CS 直接用 InstanceData 段，无消费残留）
  ✅ OnSceneConstructReady 时序修复（用户指示 + 手动确认）：MarkDirty 从
     OnSceneConstructReady（提前 3 秒，ApplyTabState/相机快照未就绪）移至
     场景构造末尾（ApplyTabState 之前——ECS 实体已全部构建后）——修复逻辑正确
⚠️ 阻塞仍在——【核心问题不在此】（用户判断：场景构建器修复逻辑正确但非根因）
```

### 11.2 依赖倒置核查（用户洞察）

```
合并后 CS 消费段变化：
  原："InstanceCulling" 段（InstanceCullingBuffer 自足——Allocate 在 Upload，dispatch 耦合）
  现："Instance" 段（frameRes 管理——m_currentFence + lastFence 保护段回收）
生命周期对比：两段都受帧 fence 保护（段在 fence 完成前不回收）——设计上安全
  → 依赖倒置设计上无问题（dispatch 提交计入 fence 需运行验证）

Octree 建立：OnSceneConstructReady called entities=6854（ready 可靠✅）、
  MarkDirty 触发重建（IsDirty triggered + dynamic extend）——重建执行但 dynamic extend 后卡死
```

### 11.3 剩余疑点（核心问题候选）

```
A. Octree 重建（AddEntity 动态扩展）卡死——dynamic extend 后无日志
   （可能无限扩容/重哈希死循环——worldSize 越界倍增无上限保护）
B. 主线程阻塞点未定位——无错误日志、dispatch 未执行（skipReady）、帧循环状态不明
C. 第三步引入的运行时差异——需对比第二步（正常）与第三步（阻塞）的
   完整数据流（Upload/SRV/CS 绑定/Octree 重建）差异

下一步方向：
  1. 定位主线程阻塞点（FrameDriver 阶段/渲染提交/fence 等待——日志时间戳分析）
  2. Octree AddEntity 扩容上限核查（防无限循环）
  3. 或回退第三步（保持第二步：CS 独立段 + InstanceData 扩展填充）——
     移动闪烁问题暂缓，先恢复可运行基线
