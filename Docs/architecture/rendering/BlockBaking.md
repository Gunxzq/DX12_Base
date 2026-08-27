# 区块烘焙方案（Block Baking）— 设计文档

> **状态（2026-08-27 更新）**：📁 设计储备（烘焙必要性大幅降低）。
> - **区块的现行定位**：CPU 空间划分粗筛的单元概念（与 `SpatialHashGrid` 空间哈希粗筛层一致）——区块作为空间划分单元仍然成立；**但烘焙的必要性不高了**（用户 2026-08-27 定案）。
> - **原因**：GPU 驱动剔除与间接绘制系统已变化——2026-08-18 无桶流程定案（`BindlessMerge_Snapshot_20260817.md` §十三）：无桶/无槽（旧版桶段区/bucketIndex/槽位数组/桶偏移表/每桶 UAV 全部废弃）；**回调同步只需拼接**，运行时轻松生成**两张段表**（culldata 段表 + 批次段表前缀和）；**实例数据是分开的**（独立于命令/CullData）——本文档所烘焙的对象（每桶 DrawArgs/桶偏移表/bucketMap/桶编号）大半已被无桶流程取代。
> - **无烘焙实现**：BlockBaker 未实施；文中各 Phase「已实施」的桶相关优化（m_bakedBucketIndices/每桶缓冲池/位掩码）随无桶流程**已回退/移出**——无界描述符表基础设施（`Texture2D gTextureMaps[]` + UINT_MAX range）**早于 GPU 驱动剔除就已存在**（纹理侧，仍保留）；桶侧无界化（每桶 UAV 数组无界化）随桶废弃。
> - **处置**：按"不删除、只标记状态"规则保留全文——被无桶流程推翻的内容以 ⚠️ 状态注记标注，烘焙设计保留为未来（若烘焙必要性回升时）的设计储备。
>
> 创建：2026-08-16
> 定案：用户确认方案可行（"InstanceData 由静态组件加持和场景预计算，本身具备缓存能力；
> 区块烘焙角度不同，方案可行。准备文档以后可以执行此部分"）。
> 目标：消除 FrameSync 中**一切场景可确定的每帧重复计算**——材质桶编号、子网格、材质段、
> 静态 DRAW_ARGS、bucketMap、偏移表、阴影项派生全数烘焙，运行时只保留动态部分。
> 关联：`CullingBlueprint.md`（GPU Driven 剔除蓝图）、`S7_ShadowCulling.md` §8.2/8.4
> （阴影材质桶段/颜色半透明阴影演进）、`RenderPipelineSpecification.md` §10.6（桶编号铁律）、
> `StaticEntityPersistentBuffer.md`（静态实体烘焙矩阵）、`BlockExpandCache_Replan.md`（块展开缓存）。

---

## 一、背景与瓶颈定位（实测）

### 1.1 FrameSync 分段耗时（引擎日志 2026-08-16 00:53 实测）

```
stg1(批次+桶号)=0.00ms   stg2(扁平化)=0.00ms
stg3(SetCullData+Apply)=0.35-0.73ms   ← 最重瓶颈（占 FrameSync 85-90%）
stg4(SetShadowArgs)=0.02-0.08ms
total=0.40-0.81ms
ShadowBuild items=2160-2651   FlattenDone allInstances=625-1094  bucketMap=2213-2702
```

### 1.2 热点根因（代码核查）

- **stg1/stg2 已并行化**（`EditorFrameAccumFlatten` Worker system，2026-08-15）——实测 0.00ms，非瓶颈；
- **stg3 热点** = `ShadowRenderItemBuilder::ApplyBucketIndices` 对 2160-2651 个阴影渲染项执行
  `entityToBucket.find(item.entity)`（unordered_map 哈希查找，~130-275ns/次 → 0.35-0.73ms）；
- **stg4** = `SetShadowBucketDrawArgs` 每桶 Map/memcpy（17-19 桶，41-73us）——小但同属"静态参数每帧重复上传"。

### 1.3 本质结论

瓶颈不在"ECS 实体遍历"（微秒级），而在 **FrameSync 中间接绘制参数预备的每帧重复计算**：
材质桶编号、子网格区间、材质段、静态 DRAW_ARGS、bucketMap、桶偏移表、阴影项派生——
这些数据**场景加载时即确定**，却在每帧重新计算/重建/查找。

---

## 二、数据面分类：可烘焙 vs 每帧必需

> ⚠️ **状态（2026-08-27）**：本表是 2026-08-16 有桶时代的分析——无桶流程（2026-08-18 定案）下，
> "每桶 DRAW_ARGS/桶偏移表/bucketMap/桶编号"这一烘焙对象已大半不存在；"回调同步只需拼接"后
> 剩余每帧 CPU 工作为段表前缀和生成（两张一维段表，运行时轻松生成）。本表保留为历史参考。

### 2.1 静态可烘焙（场景加载时一次，运行时零计算）

| 数据 | 当前每帧开销 | 烘焙后 |
|:--|:--|:--|
| **材质桶编号**（assignedBucket） | stg1 每帧递增分配 | **场景加载固化**：材质内嵌于场景 JSON（材质集合确定）+ `sceneEnvironment.renderers` 显式声明场景使用的渲染器（2026-08-16 用户定案：渲染器枚举来自外部，场景自声明其使用的渲染器子集 → 桶数量场景加载时确定）→ 桶编号分配一次 |
| **子网格 SubMeshRanges**（IndexCount/StartIndex/BaseVertex） | Builder 每帧解析 | 几何静态 → 烘焙进渲染项/桶段 |
| **材质段结构**（每材质桶段布局） | Builder 每帧构建 | 材质桶段 = 场景材质集合（JSON 内嵌），确定 |
| **SetBucketDrawArgs 静态字段** | 每桶每帧 Map/memcpy | **一次性写入 UPLOAD**，运行时仅 COPY |
| **bucketMap 扁平表**（实体→桶归属） | stg1/stg2 每帧重建（2213-2702 项） | 静态实体桶归属确定 → 烘焙 |
| **桶偏移表前缀和**（ComputeBucketOffsets） | 每帧遍历 bucketMap 全表 | 由烘焙 bucketMap 一次生成 |
| **阴影项派生**（ShadowRenderItem 材质段+桶编号） | Build + ApplyBucketIndices 每帧（含 2651 次 find） | 阴影 = 实体项 + 阴影标志派生，Build 时带固定材质桶编号 → **ApplyBucketIndices 零查找** |
| **静态实体 InstanceData**（世界矩阵/worldInvTranspose） | stg2 每帧重建 allInstances | **已有基础**：`StaticComponent`（cachedWorld/cachedWorldBounds + worldDirty 惰性缓存）+ `PrecomputedStaticData`（save 时烘焙矩阵，persistentId 索引，加载一次上传）——烘焙复用此机制 |

### 2.2 每帧必需（动态，不可烘焙）

| 数据 | 原因 |
|:--|:--|
| **CullData**（worldPos/boundingRadius/bucketOffset/bucketCount/globalInstanceIndex） | **着色器（CS gCullData t0）的运行时输入参数**（2026-08-16 用户定案：不可能走烘焙）——即使静态实体矩阵不变，bucketOffset/bucketCount 依赖运行时 bucketMap 扁平化位置、globalInstanceIndex 依赖 allInstances 顺序，整个结构每帧注入剔除层 |
| **动态实体 InstanceData**（世界矩阵） | 角色/NPC 每帧变化（无 StaticComponent 或 worldDirty） |
| **InstanceCount**（IndirectArgs[1]） | GPU 剔除 CS 原子更新，天然每帧 |
| **可见性结果**（AppendBuffer/gAppend） | 剔除产物，每帧 |

### 2.3 静态/动态边界（关键设计点）

- **静态区块**（城市建筑/树/地形，实体不动）：桶编号 + 子网格 + 材质段 + bucketMap + 偏移表 + 阴影项 + 静态 InstanceData **全数烘焙**——stg1-stg4 对这部分的计算归零；
- **动态实体**（角色/NPC，数量少）：保留逐帧路径（现有逻辑不变）；
- **混合**：烘焙的静态桶段 + 动态实体附加桶段，**桶编号空间分区**（静态 0..N 固化，动态 N+1.. 运行时分配）——桶编号无顺序性要求（见 §四），天然支持。

---

## 三、桶编号语义确认（设计前提）

### 3.1 两个"桶"概念区分（2026-08-16 用户定案，避免混淆）

| 维度 | **FrameSync 桶（L2c 批次桶）** | **材质桶（构建器区分）** |
|:--|:--|:--|
| 划分基准 | **BatchKey{geometry, materialIdx}**——实例化合并批次（同几何同材质实例合批） | **材质选定的渲染器**（shaderType → MaterialRoute.renderer：opaque/water/skinned…），RenderSlotCache.m_buckets[shaderType] |
| 粒度 | 细（批次级，含子网格段区） | 粗（渲染器路由层） |
| 决定什么 | 绘制参数段区（IndexCount/StartIndex/BaseVertex）——ExecuteIndirect 消费单元 | 哪些构建器被使用（sceneEnvironment.renderers 材质数组声明） |
| 数量 | 场景批次（City Opaque ~250） | renderers 数组长度（opaque/water…） |
| 可固化 | ✅ 是（BatchKey 惰性固化，Phase 1） | ✅ 是（renderers 决定构建器索引 → base offset） |

**关键**：FrameSync 消费的是**批次桶编号（bucketIndex）**——`gBucketMap`/桶偏移表/`item.bucketIndex`/`SetBucketDrawArgs` 全用它；材质桶（渲染器）只是路由层，不直接进 FrameSync。**FrameSync 用什么就固化什么**（用户定案）→ 固化批次桶。

### 3.2 编号语义

- **无顺序性要求**：CS 端仅按 `segBase = bucket * 5u * kMaxSubMeshRanges` 乘法索引（`InstanceCulling.cs.hlsl` 238 行）；CPU 端 `SetBucketDrawArgs` 同样按 `bucketIndex * 5 * kMaxSubMeshRanges` 定位写入（`CullingResourceManager.cpp` 407 行）——编号唯一 + 区间合法即可，**不要求连续/从 0 开始/有序**；
- **上限 1024**：`kMaxCullBuckets = 1024`，CS 越界防御 `bucket = (b < kMaxCullBuckets) ? b : 0u`（≥1024 归入桶 0）——固化编号必须在 [0, 1023]；
- **全局单点**（§10.6 铁律）：桶编号唯一分配点 = FrameSync；烘焙只是把"每帧分配"提前到"加载时分配一次"，**不改变单点约束**。

---

## 四、桶编号分层机制（构建器 base offset + 内部局部索引，2026-08-16 用户定案）

```
全局 bucketIndex = 构建器 base offset + 构建器内部局部索引
[0 .. segSize-1]                      = opaque 构建器（offset 0）
[segSize .. 2*segSize-1]              = water 构建器（offset = segSize）
[...]                                 = 后续构建器（renderers 数组顺序 × segSize）
[最后一个构建器尾 .. kMaxCullBuckets-1] = 动态实体预留
```

- **构建器 base offset**：由**材质桶（sceneEnvironment.renderers 材质数组）确定**——`段大小 segSize = kMaxCullBuckets / renderers.size()`（均分），构建器 offset = renderers 数组位置 × segSize（Opaque 通常位置 0 → offset 0）；
- **构建器内部局部索引**（2026-08-16 管道局部性定案）：`OpaqueRenderItemBuilder::m_nextBakedBucket` 惰性固化（Phase 1，跨帧稳定），**Builder 只产局部索引（0..N，不含 baseOffset）**——材质→渲染器唯一映射 ⟹ BatchKey{geometry,materialIdx} 跨构建器互斥 ⟹ 局部索引天然独立；
- **FrameSync 组装**：stg1 读 `batch.bucketIndex`（局部）+ 构建器 base offset 得全局 bucketIndex，回填渲染项 `queue[batch.queueIndex].bucketIndex`（渲染消费 argsByteOffset/GetBucketOffset 需全局）；阴影 `ApplyBucketIndices` 内加自身 offset（跟随 Opaque 段）；
- **注入链路**（2026-08-16 实施）：`EditorSceneManager::OnSceneConstructReady` 触发 `SetOnSceneConstructReadyCallback(renderers)` → Editor 回调计算 offset → `m_opaqueBuilder->SetBucketBaseOffset(offset)` + `m_shadowBuilder->SetBucketBaseOffset(offset)`；
- **多构建器**（Opaque/Water/Transparent 未来接入 GPU 剔除）：各占一段编号空间，**全局唯一且连续**——分层叠加（构建器索引 + 构建器内部索引）；
- **资源组织演进**（2026-08-16 Nanite 改造定案）：桶编号 = 每桶独立缓冲的索引键——全局 bucketIndex 直接索引每桶 Indirect Args Buffer/描述符槽位（见 §七·五），编号语义与资源组织解耦；
- **静态桶编号的确定性依据**（2026-08-16 用户定案）：
  - **材质内嵌于场景 JSON**（`dependencies.materials` / 实体 `materials[]`）——材质集合场景加载时确定；
  - **`sceneEnvironment.renderers` 数组显式声明场景使用的渲染器**（渲染器枚举本身来自外部/可扩展，但场景**自声明其使用的子集**）——渲染器集合场景加载时确定；
- 动态桶编号 = 运行时从预留区递增分配（保留现有 `assignedBucket` 递增逻辑，仅起始偏移后移）；
- 阴影桶：**复用材质桶编号**（§8.3 共享桶段结构），非独立编号空间（§10.6 禁止独立桶编号空间）。

> **兼容性说明（2026-08-16 核对）**：§10.6 "桶编号全局唯一、单点分配（唯一分配点为 FrameSync）"与"静态桶编号场景加载时分配"存在**字面张力**——铁律的**意图**是"禁止并行构建器各自分配桶编号（冲突 → CS 桶段错位）"，本方案两个分配点（场景加载主线程 + FrameSync 主线程）均非构建器/渲染器、且编号空间分区（各构建器段 + 动态预留）无重叠，**意图未违反**；但 §10.6 的字面表述需**同步修订**为："静态桶编号场景加载时按渲染器单点分配（renderers → 构建器 base offset）、动态桶编号 FrameSync 单点分配（assignedBucket 递增）"——实施 Phase 1 时应一并更新 RenderPipelineSpecification.md §10.6。

---

## 五、阴影 = 材质桶段（颜色/半透明阴影演进对齐）

`S7_ShadowCulling.md` §8.2/8.4 定案（2026-08-15）：

- 阴影渲染项粒度 → **子网格颗粒**（每材质段一项），`ShadowRenderItem` 加 `materialIndex`；
- `shadowIndirectArgs` 桶段 = **材质桶段**（每材质 InstanceCount），恢复多段；
- `DirShadowPS` 按材质类型分支：不透明→纯深度（现状）；颜色阴影→采样 baseColor×衰减；半透明→alpha 透光；
- **派生路径**：实体构建器遍历桶时顺带产出子网格阴影项（复用 base 指针 + 材质索引，零额外几何解析）——阴影项 Build 时直接带**固定材质桶编号**，`ApplyBucketIndices` 退化为"按编号放置"，**消除 2651 次 unordered_map find**（stg3 热点根除）。

---

## 六、落地形态（对齐 Bevy 预分配 + 现有架构）

> ⚠️ **状态（2026-08-27）**：本节的 BlockBaker 场景加载烘焙流程**未实施**（无烘焙实现）；
> 每帧"静态桶段直接 COPY"路径随无桶流程（2026-08-18）废弃。保留为设计储备。

```
场景加载（一次）：
  BlockBaker / SceneConstructor 扩展
    ├─ 读 sceneEnvironment.renderers（场景自声明渲染器子集）→ 分配各构建器 base offset（segSize = kMaxCullBuckets / renderers.size()，构建器索引 × segSize）
    ├─ 注入 OpaqueRenderItemBuilder::SetBucketBaseOffset（EditorSceneManager::OnSceneConstructReady 回调）
    ├─ 构建器内部 BatchKey 惰性固化局部索引（m_bakedBucketIndices，跨帧稳定）→ 全局 = baseOffset + 局部
    ├─ 烘焙 SubMeshRanges / 材质段 → 渲染项静态字段
    ├─ 烘焙 bucketMap + 桶偏移表前缀和 → UPLOAD 一次写入
    ├─ 烘焙 SetBucketDrawArgs 静态字段 → bucketArgsUp 一次写入（演进：每桶独立缓冲一次写入，见 §七·五）
    ├─ 烘焙静态实体 InstanceData（复用 PrecomputedStaticData / StaticComponent）
    └─ 派生阴影项（材质桶段 + 固定编号）→ m_shadowBucketItems 预置

每帧（仅动态部分）：
  FrameSync → 动态实体 InstanceData / CullData（CS 着色器参数，不烘焙）/ InstanceCount（CS）/ 可见性
  静态桶段：直接 COPY 烘焙参数，零计算零查找（演进：每桶独立缓冲，逐桶绑定消费）
```

### 落点（与现有模块的关系）

| 模块 | 改动 |
|:--|:--|
| `RenderSlotCache` | 桶编号固化（Rebuild/场景加载时分配固定编号，Entry 带 bucketIndex） |
| `CullingDataStore` | bucketMap/偏移表烘焙（`SetFlatInstances` 增加"烘焙模式"：静态段一次写入） |
| `CullingResourceManager` | `SetBucketDrawArgs` 静态字段一次写入（分离"静态上传"与"动态 InstanceCount"） |
| `ShadowRenderItemBuilder` | Build 时带固定材质桶编号，`ApplyBucketIndices` 退化为零查找放置 |
| `EditorFrameAccumFlatten` | 静态实体跳过重建（复用烘焙数据），只处理动态实体 |
| `Editor.cpp` FrameSync 回调 | stg3/stg4 静态部分移除（烘焙数据直接 COPY） |

---

## 七、实施阶段（文档定案后执行）

> ⚠️ **状态总注（2026-08-27）**：以下 Phase 1~5b 为 2026-08-16 有桶时代实施——
> **Phase 1/2/3/4（桶编号固化/每桶 DrawArgs 签名守卫/桶偏移表指纹守卫/阴影桶编号）**与无桶流程冲突的桶相关部分**已随无桶流程回退/移出**；
> **Phase 4.5（globalInstanceBase 循环外一次写）/Phase 5（实体级聚合段 + FrameSync 归并拼接）方向与无桶流程"回调同步只需拼接"一致——部分可能仍有效（不确定）**；
> 无界描述符表基础设施（纹理侧 `Texture2D[]` UINT_MAX range）**早于 GPU 驱动剔除即存在、仍保留**；
> 桶侧无界化（每桶 UAV 数组）随桶废弃。逐项状态见下。

### Phase 1：材质桶编号固化（2026-08-16 实施；⚠️ 2026-08-27：m_bakedBucketIndices 已随无桶流程回退——无桶/无 bucketIndex）
- 场景加载时读 `sceneEnvironment.renderers` + 场景 JSON 内嵌材质集合 → 按 renderers × 材质 枚举分配固定桶编号（单点，< kMaxCullBuckets）；
- Entry/渲染项携带固定编号；`EditorFrameAccumFlatten` stg1 静态桶不再每帧递增分配（仅动态桶）；
- **落地**：`OpaqueRenderItemBuilder::m_bakedBucketIndices` 惰性固化（BatchKey→编号缓存）+ `item.bucketIndex = bakedBucket` 构建时设置（FrameSync 回填删除）+ 分层机制（`SetBucketBaseOffset`，Editor 注入）。

### Phase 2：静态参数预上传（SetBucketDrawArgs 一次写入）（2026-08-16 实施；⚠️ 2026-08-27：m_bucketArgsSig 签名守卫属每桶 DrawArgs 体系，已随无桶流程回退/移出）
- `CullingResourceManager` 分离"静态字段"（IndexCount/StartIndex/BaseVertex，加载时一次 Map/memcpy）
  与"动态 InstanceCount"（CS 原子更新）；
- **落地**：`m_bucketArgsSig` 内容签名惰性固化（FNV-1a，同桶同内容跳过 Map/memcpy）+ `ComputeBucketArgsSig` 单一算法来源 + `item.bucketArgsSig` 构建时缓存 + FrameSync 写回循环比较跳过整个调用。

### Phase 3：bucketMap / 偏移表烘焙（2026-08-16 实施；⚠️ 2026-08-27：桶偏移表/桶段区已随无桶流程移出——运行时改为两张段表前缀和轻松生成，不再需要桶偏移表烘焙）
- 静态实体桶归属表 + 桶偏移表前缀和在加载时一次生成、一次上传；
- `SetFlatInstances` 支持静态段直接引用烘焙地址（每帧零重建）；
- **落地**：`CullingDataStore::SetFlatInstances` 桶偏移表指纹守卫（bucketMap FNV-1a，指纹相同跳过 `ComputeBucketOffsets` 每帧重算——首帧生成跨帧复用，bucketMap 变化自动重算）；`Clear` 重置烘焙状态。

### Phase 4：阴影项派生优化（消除 stg3 热点）（2026-08-16 实施；⚠️ 2026-08-27：阴影项 sitem.bucketIndex 固定编号属桶体系，已随无桶流程回退；stg3 热点本身已被无桶流程的拼接式同步消除）
- 实体构建器顺带派生子网格阴影项（固定材质桶编号）；
- `ApplyBucketIndices` 退化为按编号放置，删除 unordered_map find；
- **落地**：阴影项 Build 时携带 `sitem.bucketIndex = bakedBucket`，`ApplyBucketIndices()` 无参零查找，`m_frameShadowEntityToBucket` 全链删除。

### Phase 4.5：L2c 写回循环优化（2026-08-16 实施；2026-08-27：状态不确定——"循环外一次写"类优化方向与无桶流程一致，是否仍有效待确认）
- `item.globalInstanceBuffer` 逐项写（O(渲染项) 线性）→ **`m_frameGlobalInstanceBase` 循环外一次写**（所有桶同值，消费点读成员）；
- FrameSync 写回循环剩余（每帧 O(渲染项)）：
  - `GetBucketOffset`（查偏移表，已随 Phase 3 烘焙稳定）——必要时；
  - `ObjectCB Allocate`（CBV 地址 RingBuffer 语义，必要）；
- 该循环是随构建器线性增长的剩余项，后续可"批量 Allocate"进一步降调用次数。

### Phase 5：实体级聚合段（accum 上移构建器）（2026-08-16 实施；2026-08-27：方向与无桶流程"回调同步只需拼接、CPU 构建器生成完整绘制参数"一致——拼接式同步思想延续，具体 EntityAccumSegment 结构是否沿用待确认）
- **实体级聚合段**（管道局部性定案）：`OpaqueRenderItemBuilder` 消费桶时按实体聚合产出 `EntityAccumSegment{entity, inst, radius, buckets}`（多材质槽实体跨 BatchKey 合并桶集合）——替代 FrameSync stg1 的 accum/unordered_map 中转；
- **FrameSync stg1/stg2 归并为纯拼接**：stg1 只回填渲染项全局桶编号（`opaqueBaseOffset + batch.bucketIndex`），stg2 遍历实体级聚合段拼 CullData/bucketMap/allInstances（局部索引 → 全局编号转换）——accum/unordered_map 中转移除；
- 实例部分不依赖 CullData/桶（构建器已合并实体→实例），构建器并行产出，FrameSync 只拼（用户定案方向）。

### Phase 5b：静态实体 InstanceData 烘焙复用 ⏳（待实施）
- 复用 `PrecomputedStaticData` / `StaticComponent.cachedWorld`，静态实体每帧零重算零重传；
- 动态实体保留逐帧路径。

### 验证
- 日志对照：`[FrameSync][Diag] stg1-stg4` 各段耗时下降（实测 stg3 0.35-0.73ms → 0.04-0.06ms，FrameSync total 0.40-0.81ms → 0.08-0.16ms ✅）；
- `[ShadowSetArgs][Diag]` / `FlattenDone` 数据量不变但耗时下降；
- 视口/阴影渲染结果与烘焙前一致（无桶段错位/阴影错乱）；
- ⚠️ 2026-08-27：以上为有桶时代观测值，仅作历史基线（无桶流程下 stg 分段计时口径已变化）。
- City/City4 帧率对比（基准 53-60 FPS / 39-51 FPS，见 BlockExpandCache_Replan.md）。

## 七·五、每桶独立缓冲（Nanite 改造，2026-08-16 用户定案）

> ⚠️ **状态（2026-08-27）**：本节改造对象（单 gIndirectArgs 段区制 → 每桶独立 Indirect Args Buffer）
> 已随无桶流程（2026-08-18 定案）整体废弃——快照明确"桶资源清理 ✅ IndirectArgs/每桶 UAV/桶偏移表全部移出（CS 无桶分段）"。
> 每桶缓冲池/UAV 数组/分桶 COPY/位掩码清零均**已回退/移出**。保留为设计储备（Nanite 每桶独立缓冲思路供未来参考）。

**背景**：大一统缓冲区（单 gIndirectArgs 段区制：`segBase = bucket × 5 × kMaxSubMeshRanges` 乘法定位 + 共享桶偏移表）给资源管理带来极大复杂度（固定布局扩容灾难、桶数/容量写死、跨桶偏移维护）——改走**每桶独立 Indirect Args Buffer**（UE5 Nanite 思路）。

### 资源组织

```
每桶一个独立 Indirect Args Buffer（DRAW_INDEXED_ARGUMENTS × kMaxSubMeshRanges 段 = 5×8×4B = 160B/桶）
  └─ 桶集合场景加载确定（BatchKey 惰性固化 + 分层 offset）→ 一次性创建全部桶缓冲
  └─ 每桶缓冲：UAV（CS 原子递增 InstanceCount）+ GPU 地址（渲染 ExecuteIndirect 绑定）
  └─ 描述符：每桶 UAV/SRV 槽位按全局桶编号索引（桶集合加载确定 → 槽位一次分配）
```

### CS 剔除写入（AppendToBucket 改造）

| | 当前（单缓冲段区） | 改造（每桶独立） |
|:--|:--|:--|
| 定位 | `segBase = bucket × 5 × kMaxSubMeshRanges`（共享缓冲偏移） | **每桶 UAV 独立**（按 bucket 选缓冲，offset=0） |
| 原子计数 | 段区 InstanceCount 字段 `InterlockedAdd` | 同（每桶缓冲 InstanceCount 字段） |
| gAppend | 共享 + 桶偏移表前缀和分段 | **保留**（存活实例仍共享按桶偏移分段） |
| 桶偏移表 | ComputeBucketOffsets 前缀和（烘焙） | 保留（gAppend 分段仍需要） |

### 渲染消费（逐桶绑定）

- 渲染器遍历**渲染项队列（item 自带 bucketIndex = 消费清单，不变）** → 按 `item.bucketIndex` 选对应桶缓冲 → ExecuteIndirect 绑定该桶（offset=0，无乘法偏移）
- **构建器跨桶分组**（按渲染状态键排序，Builder 内完成）：同 PSO 桶连续 → 渲染器状态切换最少（可选优化，桶数激增时收益明显）
- **无需跨桶排序/合并**：渲染项队列即消费清单，item 即桶引用（确定性消费）

### 扩容

- 每桶独立按需分配（大桶多给、空桶不占）——消除单缓冲固定布局的桶数/容量写死约束
- 动态新增 BatchKey（运行时新材质/几何）→ 动态分配桶缓冲

### 与现有架构关系

- 桶编号语义不变（BatchKey 惰性固化 + 分层 offset）——渲染项队列仍自携带 bucketIndex
- SetBucketDrawArgs 静态字段 → 每桶缓冲一次写入（InstanceCount 留 0，CS 递增）
- 阴影复用同模式（§8.3 共享桶段结构 → 每桶独立 shadowIndirectArgs）
- 资源生命周期仍由 GpuResourceManager 统一管理（每桶缓冲按桶编号分配/释放，规则 #11 协作模式）

### 实施状态（2026-08-16 已实施）

1. ✅ `CullingResourceManager`：单 `m_indirectArgs` 拆为每桶缓冲池（`m_perBucketIndirectArgs/m_perBucketUavIndex/m_perBucketGpuAddr`，按桶编号索引，`AllocateConsecutive` 连续 UAV 槽位）+ `m_bucketMeta`（桶偏移表+非零桶区独立缓冲）+ `GetBucketIndirectArgs/GetBucketUavIndex/GetBucketIndirectArgsAddr/GetBucketIndirectArgsResource` 按桶 API + 创建/释放配对
2. ✅ CS `AppendToBucket`：`segBase` 乘法定位 → 每桶 UAV 数组（`gIndirectArgs[bucket]`，u1 连续段）+ `gBucketMeta`（u3 UAV：桶偏移表/非零桶区）+ 阴影 `ShadowAppendToBucket` 同步
3. ✅ 根签名：8 参数重映射（表 4=t3 gBucketMeta、表 5=u0、表 6=u1 UAV 数组 kMaxCullBuckets 连续段、表 7=u2）+ 主视口/阴影分支绑定适配
4. ✅ DispatchCulling：静态字段逐桶 COPY + 桶偏移表/非零桶区 COPY 到 gBucketMeta + 入口（INDIRECT→COPY→UAV）/出口（UAV→INDIRECT）对称屏障
5. ✅ 渲染消费：Editor.cpp 逐桶取资源（`GetBucketIndirectArgsResource(item.bucketIndex)`），`argsByteOffset` 恒 0；单 gIndirectArgs 并存期兼容回退
6. ✅ **阴影保持单缓冲段区（2026-08-16 用户定案：阴影 = 派生数据，不拆每桶独立缓冲）**：CS `ShadowAppendToBucket` 用 `gIndirectArgs[0][segBase+...]`（单 UAV 数组起点 + 段区乘法，匹配阴影单缓冲 ShadowIndirectArgs[li]）——与主视口每桶数组（`gIndirectArgs[bucket]`）分离语义；阴影 dispatch 表 6 绑 `GetShadowIndirectArgsUavIndex(lightIndex)`（单 UAV 到数组起点）+ 渲染消费段区偏移（Editor.cpp:2153）一致
7. ❌ **构建器跨桶分组（取消，2026-08-16 用户定案）**：已做每桶独立缓冲，渲染项队列自携带 bucketIndex 确定性消费——跨桶分组/排序不必要（§七·五 渲染消费段）

## 七·六、非空桶过滤策略（2026-08-16 用户定案：方案 A 位掩码）

> ⚠️ **状态（2026-08-27）**：位掩码（gBucketMask）针对 1024 桶逐桶 COPY 的浪费，已随无桶流程
> （桶资源全部移出）**回退/移出**。下"Web 检索评估"（Nanite GPU compaction/EA SEED 位掩码/Frostbite
> tile bucket）保留为工业标准参考。

### 问题背景（实测）

DispatchCulling 每帧**全量 1024 桶**逐桶 COPY 静态字段（录制命令 `CopyBufferRegion 1037 次/帧`，`[DispatchRec] main dispatch avg=5.7-6.7ms`）——每桶独立缓冲改造的录制成本成为 CPU 压力主体；但**实际非零桶仅 ~113**（ExecuteIndirect 71-76 次），89% 的 COPY 是空桶浪费。

### Web 检索评估（2026-08-16，方案可行性验证）

| 来源 | 做法 | 结论 |
|:--|:--|:--|
| **Nanite（GDC 2024 Wihlidal / SIGGRAPH 2021 Karis）** | **GPU 侧 compaction**（empty dispatch compaction——只启动有工作的 bin）；bitmask+atomic OR 用于**软件光栅化 quad/pixel 筛选**（GPU 内） | 最新实践 = 避免 CPU 回读；但改动大 |
| **EA SEED Coverage Bitmasks（Mittring）** | 位掩码 + **CPU 端遮挡剔除**（Crytek 2007 时代）；明确"readback GPU z-buffer 加延迟一帧以上" | 位掩码技术基础成立（CPU 端先例） |
| **Frostbite（光照 tile bucket）** | `firstbitlow` + `WaveActiveBitOr` 遍历非空 bucket（**GPU 内**） | 位掩码 + 位扫描是工业标准操作 |

**结论**：位掩码 + `InterlockedOr` + `firstbitlow` 位扫描是**工业标准技术**（Frostbite/EA SEED/Nanite 均用）；但大型引擎最新实践（Nanite）用 **GPU 侧 compaction** 而非 CPU 回读。**方案 A（位掩码 CPU 回读 32 DWORD）改动小、立即落地**，且回读量远小于 InstanceCount 回读（避坑：绝不读 InstanceCount 过滤——PCIe 同步）。

### 方案 A：位掩码（当前选定，2026-08-16 用户定案）

```
DEFAULT UAV：gBucketMask（RWStructuredBuffer<uint>，kMaxCullBuckets/32 = 32 uint，GPU InterlockedOr 写）
READBACK 副本：m_bucketMaskReadback（32 uint = 128B，每帧 1 次 CopyBufferRegion）

GPU（CS AppendToBucket，实例可见处）：
  InterlockedOr(gBucketMask[bucket >> 5], 1u << (bucket & 31));   // 标记非空桶位

CPU（DispatchCulling 后）：
  1 次 COPY：DEFAULT → READBACK（32 uint）
  位扫描（__builtin_ctz / _BitScanForward）：遍历非空桶 → 只清这些桶 InstanceCount
  分帧重置：DEFAULT binMask 每帧 dispatch 前清零（OR 累积残留）；CPU 遍历 READBACK 后顺手清零

收益：静态字段 COPY 1037 → ~113 次（dispatch 录制 5.7-6.7ms 大幅下降）；消费侧可按非空桶跳过空 ExecuteIndirect
```

### 方案 B：GPU 侧 compaction（未来参考，Nanite 做法）

GPU 端压缩命令缓冲（只启动有工作的 dispatch/draw，间接 dispatch 链）——完全避免 CPU 回读；改动大，桶数激增时再演进。

### 决策

**当前走方案 A（位掩码）**——技术标准、改动小、立即消除 1024 桶 COPY 浪费；B（GPU compaction）留作桶数激增时时的未来演进。

## 七·七、无界数组桶段改造（几何-PSO 桶，2026-08-16 评估定案）

> ⚠️ **状态（2026-08-27）：本节需区分两半**——
> ① **无界描述符表基础设施（`Texture2D gTextureMaps[]` + UINT_MAX range + ENABLE_UNBOUNDED_DESCRIPTOR_TABLES）：
> 早于 GPU 驱动剔除就已存在**（用户 2026-08-27 确认），纹理侧仍保留、仍在使用；
> ② **桶侧无界化（`gIndirectArgs[] : register(u4)` UAV 数组 UINT_MAX + 每桶缓冲动态扩容）：随桶废弃/已回退**。
> 实施状态条目的有效范围据此判定。

### 背景与方向

用户指正（2026-08-16）：**无界数组桶段（几何-PSO 桶 = 材质等价渲染器）优先级高于桶并行上传**——且 GPU 驱动剔除前 PSO 已遵循无界数组纹理运用模式（`Texture2D gTextureMaps[] : register(t0, space2)` + 根签名 `UINT_MAX` range + `D3DCOMPILE_ENABLE_UNBOUNDED_DESCRIPTOR_TABLES`，Game/Editor 共用）。

### 旧模式参考（已验证基础设施）

| 项 | 旧模式（无界数组纹理） | 位置 |
|:--|:--|:--|
| 声明 | `Texture2D gTextureMaps[] : register(t0, space2)` | Common_PBR.hlsl:37（color.hlsl include） |
| 根签名 | `Init(SRV, UINT_MAX, 0, 2, OFFSET_APPEND)` | BillboardRenderer.cpp:168 / EnvBillboard:264 / ReflectionProbe:103 |
| 编译 | `D3DCOMPILE_ENABLE_UNBOUNDED_DESCRIPTOR_TABLES` | 各渲染器 flags |

### 可行性评估（2026-08-16）

**技术可行**：
- 无界 **UAV** 数组 = 已验证无界 **SRV** 数组（3 渲染器）的 UAV 同构——D3D12 支持 UAV range `UINT_MAX`
- 基础设施齐全：`AllocateConsecutive` 连续段（DescriptorHeapCollection.cpp:162/275）+ 编译 flags + 9 参数根签名（回退后稳定）
- 当前 `gIndirectArgs[] : register(u4)` 已是数组语法——只需根签名 range 有限 → `UINT_MAX`

**风险**：
1. **Intel 集显驱动对 unbounded UAV range 的支持需实测**（SRV UINT_MAX 已验证，UAV 是新增点；曾有 10 参数根签名崩溃史——但无界改造**保持 9 参数**（只改 range），不新增参数，风险可控）
2. **桶缓冲动态分配**配合：当前固定 kMaxCullBuckets 预分配（1024）→ 无界 range 后桶数超限需按场景实际桶数动态分配（复用 AppendBuffer ResizeAppendBuffer fence 延迟释放模式）

### 实施状态（2026-08-16 已全部完成，运行时无异常；⚠️ 2026-08-27：阶段 1/2/3 属桶侧改造，随无桶流程废弃/回退）

1. ✅ **阶段 1（最小改造）**：根签名 `ranges[7].Init(UAV, UINT_MAX, 4)`——保持 9 参数 + 槽位 u4，CS/绑定/屏障不动；Intel 集显无崩溃（unbounded UAV range 兼容确认）
2. ✅ **阶段 2（桶缓冲动态）**：`m_perBucketIndirectArgs` 按实际桶数分配（`actualBucketCount = max(bucketMap)+1`，空兜底 1）+ 扩容（`ResizePerBucketBuffers`——新 AllocateConsecutive 连续段 + 旧段/旧缓冲 fence 延迟释放，对齐 ResizeAppendBuffer #921）+ 触发点（SetFlatInstances 检测桶增长）；位掩码区 uint 数动态（`bucketMaskUints = ceil(实际桶数/32)`）
3. ✅ **阶段 3（CS/屏障适配）**：CS `gIndirectArgs[bucket]` 索引无界化（阶段 1 range UINT_MAX 已覆盖）；静态引用桶屏障遍历上限 kMaxCullBuckets → 实际桶数（三处循环）；位掩码区/maskBuf 动态化（`GetBucketMaskUints` 访问器 + `std::vector` 防溢出）
4. ✅ **阶段 4（验证）**：编译 + 运行无异常（用户确认 2026-08-16）——CreateComputePipeline 无崩溃 + dispatch/渲染无回归（City 71-76 桶）

### 后续演进（未来，未实施）

- **桶 = 几何-PSO 桶段完整化**：桶段内嵌 PSO 索引（gBucketMeta 加 PSO 槽位）→ 桶编号与渲染器（RenderSlotCache 材质槽 shaderType → 渲染器）统一语义落地
- **Game 端接入 GPU 驱动剔除**：Game 端共用 OpaqueRenderer + 无界数组基础设施已就绪，接入时桶段无界改造直接复用
- **阶段 2b 边界**：异步加载期桶增长扩容已实现（ResizePerBucketBuffers），超大量桶（>描述符堆段容量）需评估描述符堆分区扩展

### 收益

- 桶数脱离 kMaxCullBuckets（1024）硬限制——无界动态扩展
- **桶 = 几何-PSO 组合（BatchKey{geometry, materialIdx}）→ 天然符合"材质等价于渲染器"**（RenderSlotCache 材质槽 ↔ 桶编号统一语义）
- 复用已验证无界数组纹理模式（Game/Editor 共用基础），模式复用非新模式

---

## 八、与既有定案的一致性

| 既有定案 | 本方案 |
|:--|:--|
| §10.6 桶编号全局单点、禁止独立编号空间 | ⚠️ **意图兼容、字面需修订**：静态桶场景加载单点分配 + 动态桶 FrameSync 单点分配（均主线程、编号分区无重叠、非构建器分配）——实施 Phase 1 时同步更新 §10.6 表述；阴影复用材质桶编号（非独立空间）✅ |
| §10.6 禁止构建器独立偏移表 | ✅ 偏移表仍 `CullingDataStore` 全局一份（烘焙生成一次） |
| S7_ShadowCulling.md §8.2/8.4 材质桶段演进 | ✅ 本方案 Phase 4 直接落地（阴影 = 材质桶段 + 固定编号） |
| StaticEntityPersistentBuffer.md 静态烘焙矩阵 | ✅ Phase 5 复用 |
| 块展开缓存（BlockExpandCache_Replan.md 2026-08-11） | ✅ 互补：块展开缓存 = 成员 Entry 展开；区块烘焙 = 参数预备固化 |
| **每桶独立缓冲（Nanite 改造，2026-08-16 用户定案）** | ✅ 新定案：单 gIndirectArgs 段区制 → 每桶独立 Indirect Args Buffer（§七·五）——桶编号语义不变（渲染项队列自携带 bucketIndex = 消费清单），资源组织解耦（每桶 UAV/缓冲/描述符按桶编号索引），CS 原子计数/桶偏移表/gAppend 分段语义保留 |

---

## 九、待定项与待办

### 已完成（2026-08-16 renderers 落地 + 分层机制实施；⚠️ 2026-08-27：分层 base offset 机制属桶编号空间，已随无桶流程废弃——`sceneEnvironment.renderers` 字段（场景自声明渲染器子集）作为场景元数据是否保留待确认）
- ✅ **`sceneEnvironment.renderers` 字段已落地**：`SceneEnvironment` 新增 `std::vector<std::string> renderers` + `from_json/to_json` 两端（`SceneDescription.h`）+ `Schemas/scene.schema.json` sceneEnvironment properties 新增 `renderers` 数组 + **City 场景文件已写入** `"renderers": ["opaque", "water"]`（材质路由名：MaterialRoute.renderer）。
- ✅ **分层机制已实施**（2026-08-16 用户定案）：`OpaqueRenderItemBuilder::SetBucketBaseOffset` + `bakedBucket = baseOffset + 局部索引`（惰性固化）；`EditorSceneManager::OnSceneConstructReady` 触发 `SetOnSceneConstructReadyCallback(renderers)` → Editor 注入 opaque offset（`segSize = kMaxCullBuckets / renderers.size()` × 构建器索引）——多构建器各占一段全局唯一连续编号空间。

### 待办（实施中记录）
1. **序列化写入 renderers（Editor 端）**：`ExportToDescription`（EditorSceneManager）保存场景时写入 `sceneEnvironment.renderers`（从运行时实际使用的渲染器收集）——**用户定案：暂时不考虑序列化问题，记录待办**；
2. **旧场景从材质数据解析回退**：旧场景无 `sceneEnvironment.renderers` 时，从场景 JSON 内嵌材质数据（材质 shaderType → MaterialRoute.renderer）解析渲染器集合回退——用户定案方向：旧场景可从材质数据解析到；
3. **动态桶编号预留区大小**：`[静态桶数 .. 1023]` 余量分配，动态实体极多时需扩容策略；
4. **静态实体判定**：`StaticComponent` 存在且 `!worldDirty` 即静态（现有语义），是否需场景级 `entityMotionPolicy` 参与。

