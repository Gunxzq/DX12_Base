# GPU Driven 剔除分层蓝图（2026-08-06 定稿）

> 日期：2026-08-06
> 状态：📋 历史设计蓝图（2026-08-06 分层定案；L2 数据流等实现路径已被 2026-08-18 无桶流程取代，保留为历史）
> **阶段定位**：S0 总蓝图——三层模型 L1（S1 CPU 粗筛）/ L2（S5 GPU 剔除 CS）/ L3（S5 HZB 遮挡）。
> 现行无桶流程阶段表（S1~S6 + S7 阴影）见本目录 `README.md`
> 关联：`08_MapScenePipeline.md` §八（区块化聚合，500 单位块 + 实例矩阵数组）、
> `../culling/OctreeCullingAndRaycaster.md`（空间哈希 + CulledSet）、
> `BugFix_Editor_StaticDirtyState_And_OutlinerIssues.md` §六（CPU 剔除定稿）、
> `BillboardSystemArchitecture.md`（公告牌/树 = 交叉 quad）
>
> **2026-08-10 定案修订（集群 ≠ 区块，职责分离）**：
> - **集群（BlockComponent）**：内容逻辑分组 + 大包围盒 + forceVisible（对抗远近裁剪面，UE
>   bAlwaysVisible / HLOD Cluster 参照）——**场景构建器不再生产 BlockComponent**（编辑器字段/
>   schema 未准备，集群功能暂缓，不一定会使用）。
> - **区块（空间哈希块）**：标准自动化剔除块，**由剔除层空间哈希模块生成**（`blockConfig` 驱动，
>   缺失自动推导），格存**成员实体 ID**（CulledSet::Entry 中的成员实体）。
> - **块展开缓存归剔除层**：块→成员展开（原 RenderSlotCache::m_blockExpanded）迁出至
>   `CullingLayer`（SpatialHashGrid/CullingDataStore），RenderSlotCache 回归纯材质槽分桶。
> - 详见本文档 §4.3（2026-08-10 定案）。
>
> 背景：City4 场景 15489 个 ECS 实体（tree 8404 + tree5/tree2 3383 + 广告牌 2798 + 建筑 ~900），
> 94% 是树与广告牌；树本质是"略微旋转的交叉 quad"（~24 顶点/14 三角形），公告牌表达成立，
> 是实例化 + GPU 剔除的理想输入。

---

## 零、核心认知澄清（先纠偏，再谈层级）

| 说法 | 实际 |
|:--|:--|
| "实体数量级减少了" | ✅ **ECS 实体数确实减少**：15489 → 5 块（BlockComponent 聚合，-99.97%，`08_MapScenePipeline.md` §8.4 已定案；⚠️ 2026-08-10 修订：聚合归空间哈希模块生成，见 §4.3） |
| "实体数量级实际上并没有减少" | ✅ 也成立——**渲染实例数据量没有减少**：块内实例矩阵仍是 15489 个（只是 SOA 压缩 252KB），数据表达层不变 |
| "构建器那边的精细剔除下放给 GPU" | ✅ **对，这就是本蓝图的核心**：原 Builder 内部逐实体视锥测试（`Frustum::Contains`，CPU）→ 下放为 GPU Compute 逐实例剔除 |

**结论**：聚合解决的是 **ECS 实体遍历/get/管理开销**（15489 实体 → 5 块）；GPU 剔除解决的是 **15489 个实例的逐实例视锥测试位置**（CPU Builder → GPU Compute）。两者正交，先后衔接。

---

## 一、期望层级划分（三层模型）

```
L1 CPU 块级粗筛（✅ 已有，2026-08-04 定稿）
  空间哈希（格子 250）→ 块实体视锥剪枝 → cullDistance 拒远 → CulledSet
    → 常态仅 5 个块实体（City4），相机静止缓存复用
        │
        ▼
L2 GPU 实例级精筛（❌ 待建，本蓝图主体）
  可见块的实例矩阵 StructuredBuffer（静态加载时一次上传）
    → Compute 逐实例视锥剔除（每实例包围球）
    → AppendBuffer / Atomic 计数 → IndirectArgs
    → DrawIndexedInstancedIndirect 一次提交
        │
        ▼
L3 遮挡剔除（📋 2026-08-12 提前，原远期）
  HZB 深度遮挡——**双时域消费**：本帧 SSR/接触阴影（Lighting 阶段）+ 下一帧遮挡剔除（PrePass）
  保守化 PVS（只剔确定被遮挡的小物体，排除地块——上版 PVS 失败教训见
  `BugFix_Editor_StaticDirtyState_And_OutlinerIssues.md` §五）
```

| 层 | 粒度 | 内容 | 执行者 | 频率 | 状态 |
|:--|:--|:--|:--|:--|:--:|
| L1 | 块实体（≤5） | 空间哈希/视锥/cullDistance → CulledSet | CPU | 每帧（静止缓存） | ✅ |
| L2 | 块内实例（~4611/块） | 矩阵上传（静态一次）→ compute 剔除 → 间接绘制 | GPU | 每帧（仅可见块） | ❌ |
| L3 | 实例 | HZB 深度遮挡（双时域：本帧 SSR/接触阴影 + 下一帧遮挡剔除）/ 保守化 PVS | GPU | 每帧/场景级 | 📋 |

---

## 二、动静分流（按"数据稳定性"分流，不是按"交互性"）

参考大型引擎（UE GPUScene/Nanite、Unity BRG）的共识：**静态 = 变换/材质/包围体稳定**，
交互（如建筑损伤切换）走渲染项替换，不改变剔除路径。

| 内容 | 数量 | 归类 | 路径 |
|:--|:--:|:--|:--|
| 树（tree/tree5/tree2/treebig，交叉 quad） | ~11787 | 纯静态 | **L2**：块内实例矩阵一次上传 + GPU 剔除 + 间接绘制 |
| 广告牌（海报类） | 2798 | 纯静态 | **L2**（可并入树路径） |
| 建筑（bill00~08） | ~900 | 半静态（损伤状态可变 `_d`） | 静态实体路径：矩阵持久化，状态切换走渲染项/材质替换；900 个 CPU 视锥足够，不必进 L2 |
| 机体/子弹/特效 | <50 | 动态 | CPU 视锥 + 每帧上传；被静态世界遮挡（L3 远期） |

### 交互与不交互的协调

1. **共享 CulledSet 分发**：所有层从 L1 的 CulledSet 出发——静态块、半静态实体、动态实体共用粗筛结果，避免各层重复遍历。
2. **静态层不受交互影响**：树/建筑矩阵加载时烘焙进持久 GPU 缓冲；建筑损伤只更新"渲染项索引"（4 字节槽），不重传矩阵。
3. **动态层每帧叠加**：机体等在静态绘制后按普通路径画，被静态世界遮挡但从不参与预计算遮挡。
4. **相机静止缓存继续有效**：ViewProj 未变时 L1 coarse 与 L2 剔除结果均可复用（已有先例）。

---

## 三、L2 数据流（待实现）⚠️ 具体形态已被无桶流程取代

> **2026-08-27 注**：本节为 2026-08-06 蓝图期 L2 数据流（AppendBuffer 存活实例 ID + 每桶 IndirectArgs）。
> 实际实现演进为：桶流程（L2c）→ **2026-08-18 无桶流程**——CPU 构建器按子网格产出完整绘制参数
> （每 BatchKey 一条间接命令、`CullData.groupId` = 段内命令索引，CS 只原子累加 InstanceCount；
> culldata 段表 + 批次段表两张 SRV + 一次 `ExecuteIndirect`）。"GPU 剔除 → 间接绘制"主线仍有效，
> 下方数据流与缓冲布局要点为历史形态。详见 `Docs/snapshots/BindlessMerge_Snapshot_20260817.md` §十三、
> `../rendering/RenderPipelineSpecification.md`。

```
BlockComponent（5 块，持实例矩阵数组，SOA 252KB）
  │  加载时（仅一次）
  ▼
实例矩阵 + 实例包围球半径 → StructuredBuffer（StaticEntityPersistentBuffer 模式）
  │  每帧（块进入 CulledSet）
  ▼
Compute 剔除 pass（每实例一个 thread）
  ├─ 包围球 vs 视锥（半径 = 网格 localBounds × 实例缩放，转换器侧算好）
  ├─ 可选：距离 LOD 选择（tree 近/远）
  ▼
AppendBuffer / Atomic 计数 → IndirectArgs
  ▼
DrawIndexedInstancedIndirect（一次提交，CPU 无需知道存活实例数）
```

### 缓冲布局要点

| 缓冲 | 内容 | 更新 |
|:--|:--|:--|
| InstanceBuffer | 15489 × 矩阵（+ 半径 + 可选 LOD 层级） | 加载时一次（静态） |
| 剔除结果位图/AppendBuffer | 存活实例 ID | 每帧（仅可见块） |
| IndirectArgsBuffer | 绘制参数 | 每帧由 compute 生成 |

---

## 四、与现有架构的衔接

| 现有组件 | 关系 |
|:--|:--|
| `SpatialHashGrid`（空间哈希） | **区块生成与存储的归属模块（2026-08-10 定案）**：按 `blockConfig` 自动划分区块，格存**成员实体**（CulledSet::Entry 的成员实体 ID）——取代旧"块实体入格"方案 |
| `CullingSystem` / `CulledSet` | 仍是唯一入口：命中区块 → 展开成员实体 → 上传/剔除；未命中 → GPU 不碰 |
| `RenderSlotCache` | **回归纯材质槽分桶（2026-08-10 定案）**：消费成员实体，`m_blockExpanded`（块展开缓存）迁出至剔除层 |
| `OpaqueRenderItemBuilder` 等 | 移除逐实例视锥测试（`Frustum::Contains`），只消费剔除层输出的成员实体结果 |
| PVS | 降级为远期 L3 可选项，必须保守化重做（只剔小物体，排除地块） |

### 4.1 查询单元与数据组织单元对齐（2026-08-06，`08_MapScenePipeline.md` §8.7）

> 审视结论（用户定案）：**空间哈希查询单元 ≠ 块数据组织单元，需要解耦 + 块可配置**。
> 现状代码三套粒度脱节：空间哈希格子（`SpatialHashGrid::m_cellSize=250` 硬编码）、
> BlockComponent 集群（Phase C 500 单位）、资产侧聚合块（§8.5 定案 500 单位）——三者各自固定。

| 单元 | 角色 | 归属 | 配置方式 |
|:--|:--|:--|:--|
| 空间哈希格子（查询单元） | 视锥剪枝粒度，格子级跳过 | 引擎（`SpatialHashGrid::m_cellSize`） | 引擎默认 250，可随块联动 |
| **块 cellSize（数据组织单元）** | 块实体划分、实例矩阵归属、L2 剔除输入粒度 | **场景（`blockConfig`）** | **JSON 可配置；缺失时加载推导** |
| BlockComponent 集群 | 内容逻辑分组（大包围盒 + forceVisible，UE bAlwaysVisible） | 场景/编辑器 | 与 cellSize 正交，可叠加 |

**原则（UE World Partition 模式）**：
1. **块大小由场景 JSON 决定**（`blockConfig.cellSize` / `blocksPerAxis` / 上下限），引擎不硬编码
2. **缺失时初始化加载推导**：`cellSize = clamp(mapExtent / blocksPerAxis)`，`blocksPerAxis` 默认 4（对齐 §8.3 实测甜点）
3. **查询单元解耦**：空间哈希格子可保持 250 或随块大小联动；块跨格子正常（格子级视锥剪枝仍生效）
4. **四端一致性**（规则 #23）：`BlockConfigDesc`（SceneDescription.h）+ `ParseBlockConfig`（SceneLoader.cpp）+ SceneConstructor 推导/复用 + `ExportToDescription` 固化写回——详见 `08_MapScenePipeline.md` §8.7.2

### 4.2 集群职责收缩：纯剔除豁免器（2026-08-06 定案）

> 审视结论（用户定案）：**在"实例 = 实体"（块实体持实例矩阵数组）的形态下，集群（BlockComponent）
> 不再是"对抗实体数"的工具——实体数已由块聚合解决（15489→5）。集群组件只剩一个职责：
> **对抗远近裁剪面**——大包围盒 + forceVisible 强制可见**。与 `Block.h` 注释"集群 ≠ 区块"的定位一致。

| 组件 | 对抗对象 | 手段 | 与 L2 的关系 |
|:--|:--|:--|:--|
| 区块（块实体，cellSize） | 实体数量级 | 实例矩阵数组聚合 | L2 的输入来源 |
| **集群（BlockComponent）** | **远近裁剪面** | clusterBounds 大包围盒 + forceVisible | **正交可叠加** |

**关键澄清（可叠加性）**：`forceVisible` ≠ "块内所有实例都画"——它只让**块实体进入候选集**
（粗筛豁免，对抗远裁剪），块内实例的 L2 GPU 视锥剔除**照常进行**。对齐大型引擎：

| 大型引擎 | 插入点 | 我们 |
|:--|:--|:--|
| UE `bAlwaysVisible`（跳过视锥测试，遮挡查询仍可剔除） | `ComputeViewVisibility()` 视锥循环内 | `BlockComponent.forceVisible` → `SpatialHashGrid::m_forceVisibleEntities`（查询时直通候选集，✅ 已实现） |
| UE HLOD Cluster 大包围体（远距合并代理） | World Partition 流式后、视锥剔除前 | `clusterBounds` + `FrustumCullAABB` 15% 扩展（✅ 已实现） |
| UE 远平面 / cull distance volume / 自适应远平面 | 相机矩阵构建时 | `AdaptiveFarPlane.md`：CullFarPlane=4500 宽剔除平面 / FarPlane=2500 渲染上限（✅ 已实现，2026-08-12 定案双裁剪面分离） |

```
集群 forceVisible（对抗远裁剪）→ 块进候选集（粗筛豁免，不被远平面裁掉）
        ↓ 叠加
L2 GPU 实例剔除（对抗视角方向）→ 块内实例逐个视锥，背面/视锥外实例仍被剔掉
```

**适用对象**：山、远距建筑群、地形边界等"内容太大，超出裁剪面仍应可见"的内容（用户定案：
"山不应被远裁剔除——地形边界内容不适用远近裁剪"）。小块树/广告牌**不需要** forceVisible——它们靠块级粗筛 + L2 GPU 剔除即可。

**集群成员构成定案（2026-08-11 用户补充）**：集群的目的 = **无视远近裁剪面的可见**（对抗远裁剪），
因此**必然只包含大型物体**（小物体靠块级粗筛 + L2 GPU 剔除即可，无需豁免）。成员构成原则：

- **按集群边界"围一层"**：只收录**大型 + 位于块边界**的物体——足够撑起 clusterBounds 大包围盒
  对抗远裁剪即可，**不包含块内所有内容**；
- **集群成员数 < 块成员数**（实际内容比单纯块反而更少）：块包含 cellSize 内全部实体，集群只
  收录边界大型物体——**集群的查询/展开压力通常比块更小**；
- 与块正交可叠加：集群实体可**跨越多个空间区块**（边界围层天然跨块），集群大包围盒一次豁免
  进候选集，块内成员照常走 L2 GPU 剔除——两条路径独立贡献候选集，GTA 去重兜底。

> 因此：集群 ≠ 块的子集关系，而是**"块的边界壳 + 大型物"**——成员构成上比块更稀疏，压力更小，
> 职责纯粹（只对抗远裁剪，不重复块的空间划分职责）。

### 4.3 集群/区块职责分离定案（2026-08-10 修订，2026-08-11 终版）

> 用户定案：**集群（BlockComponent，forceVisible）与区块（空间划分块）是两个正交概念，不得混用**。
> 历史实现把"区块生成"逻辑（Phase C 按 cellSize 分组）直接产出了 BlockComponent，导致
> 集群（内容分组/forceVisible）与区块（空间划分）被强制 1:1 绑定——与 `Block.h` 注释
> "集群 ≠ 区块，可叠加"的定位相悖。

| 维度 | 集群（BlockComponent） | 区块（空间划分块） |
|:--|:--|:--|
| 语义 | 内容逻辑分组（大包围盒 + forceVisible，对抗远近裁剪面） | 空间划分剔除块（对抗实体数量级，L2 输入粒度） |
| 生成 | **场景构建器不生产**（编辑器字段/schema 未准备，功能暂缓） | **空间索引构建时按 `blockConfig` 分组生成**（cellSize 驱动，缺失自动推导） |
| 存储 | 集群组件本体（可选，不一定会使用） | **块实体 + 块组件（ECS 组件）**——块实体入格（clusterBounds 大包围盒） |
| 块成员 ID | — | **驻留块组件 memberEntities**（块 = 成员 ID 集合；不预展开进 CulledSet） |
| 块展开缓存 | — | **归渲染管线侧**（`RenderSlotCache::m_blockExpanded`，Rebuild view<BlockComponent> 构建） |
| RenderSlotCache | 不感知 | 持有块展开缓存（对齐 UE FMeshDrawCommand），Dispatch push 指针零拷贝 |

**2026-08-11 终版要点（参考大型引擎 UE FMeshDrawCommand CachedMeshDrawCommands + 用户定案）**：

1. **块 = ECS 组件（区块组件，非集群）**：块实体持 `memberEntities`（块中成员 ID 集合）+
   `clusterBounds` + SceneTagComponent。**查询/使用效率与好处**：
   - `RenderSlotCache::Rebuild` 直接 `view<BlockComponent>` 构建展开缓存（零注入，去 Editor 手动同步）；
   - 生命周期统一（随场景 CRUD → MarkDirty，随 SceneTagComponent 销毁）；
   - 任意 system 可 `TryGetComponent<BlockComponent>` 查块归属（拾取/编辑器/调试）。

2. **不预展开（简洁引用）**：CulledSet 块条目 = `{blockIndex, clusterBounds, sceneId}`（≤5 条，
   不携带成员 ID）。**理由**：预展开每帧 124KB×3 阶段拷贝（Query→Merge→Cull），而 Builder
   并行消费桶（`ForEachBucket`）根本不读 CulledSet——拷贝是纯浪费。块成员 ID 驻留块组件，
   CulledSet 只引用（blockIndex）。

3. **展开缓存归渲染管线侧**（大型引擎结论）：UE CachedMeshDrawCommands 在 **AddToScene（变更时）
   预构建驻留、每帧只选命令**——块展开缓存（块→成员分桶 Entry）同此形态：Rebuild（变更驱动）
   构建驻留，Dispatch 每帧引用。剔除层只做块可见性判定。

4. **查询效率分析（City4 超大地图）**：
   - **块实体入格（clusterBounds 大包围盒）**：查询按格子粗筛命中块 → **O(命中格子数)**，
     **非 O(总块数)**——块数再多（City4 大图数千块）查询成本只随视锥覆盖格数增长，不随块数退化；
   - Dispatch 展开：`m_blockExpanded[blockIndex]` **数组下标 O(1)**（块数 ≤ 命中数），每帧 ≤ 命中块数
     次数组访问 + push 指针，远优于预展开拷贝；
   - 与 §4.1 解耦原则一致：块 cellSize（数据组织单元）由 blockConfig 决定，查询单元（格子 250）
     保持解耦，块跨格子正常。

**代码现状（2026-08-11 文档定案，待实施）**：本 §4.3 为终版设计——块作为 ECS 组件 + 不预展开 +
展开缓存归渲染管线侧。历史 2026-08-10 实施的"纯数据 m_blocks + 查询展开成员"方案因预展开
拷贝浪费（帧率回退至 15-18 FPS）废弃。实施清单见 `Docs/todos/BlockExpandCache_Replan.md`。

**双重归属（实体既在可见区块、又在集群中）**：大型引擎本就支持集群概念（UE bAlwaysVisible /
HLOD Cluster）。两种归属最终都汇入同一 `CulledSet`（块条目级），由 **GTA 查询计数器去重**
（SpatialHashGrid 已有 `m_queryStamps`，O(1)）——区块（空间）与集群（内容）各自独立贡献候选集，
内容分组可跨越多个空间区块（建筑群跨 2×2 区块时集群大包围盒一次豁免）。

---

## 五、实施阶段（可行性步骤，L0 → L3）

### 阶段 0：块配置化四端（`08_MapScenePipeline.md` §8.7.2）✅ 已完成 2026-08-06

| 步骤 | 内容 | 状态 |
|:--|:--|:--:|
| 0a | `SceneDescription.h` 新增 `BlockConfigDesc`（cellSize/blocksPerAxis/minCellSize/maxCellSize）+ `to_json`/`from_json`；`SceneDescription::blockConfig` 可选字段 | ✅ 2026-08-06 |
| 0b | `SceneLoader.cpp` 新增 `ParseBlockConfig`（`j.contains("blockConfig")`，缺失 = 推导模式）+ SaveToJSON 输出 | ✅ 2026-08-06 |
| 0c | `SceneConstructor` 加载推导：`cellSize = clamp(mapExtent / blocksPerAxis)`，复用 Phase C 划分逻辑；Phase C 消费 `blockCellSize`（`EditorSceneManager.cpp`） | ✅ 2026-08-06（待人工编译验证） |
| 0d | `ExportToDescription` 保存时固化 `blockConfig`（`SceneSnapshot` 缓存 + 写回，下次加载零重算） | ✅ 2026-08-06 |
| 0e | `Schemas/scene.schema.json` 加 `blockConfig` 定义（4 字段，JSON 校验通过） | ✅ 2026-08-06 |

### 阶段 1：区块级粗筛落地（L1，2026-08-10 按 §4.3 修订）

| 步骤 | 内容 | 状态 |
|:--|:--|:--:|
| 1a | 空间哈希模块按 `blockConfig` 生成区块，格存**成员实体**（CulledSet::Entry 成员实体 ID——取代旧"块实体入格"方案） | ✅ 2026-08-10（Editor.cpp OctreeCulling 分组生成） |
| 1b | 块→成员展开缓存归剔除层（SpatialHashGrid/CullingDataStore），RenderSlotCache 消费成员实体（`m_blockExpanded` 迁出，回归纯材质槽） | ✅ 2026-08-10（SetBlocks + 查询展开） |
| 1c | 验证区块级 CulledSet → 成员实体 → 桶 → Builder 链路 | 📋 待人工编译验证 |

### 阶段 2：集群豁免接入（§4.2，纯剔除豁免器）

| 步骤 | 内容 | 状态 |
|:--|:--|:--:|
| 2a | `forceVisible` 直通候选集：✅ **已实现**（`SpatialHashGrid::m_forceVisibleEntities`）——验证山/远距建筑群走豁免 | ✅/验证 |
| 2b | `clusterBounds` 大包围盒 + `FrustumCullAABB` 15% 扩展：✅ **已实现**——验证"超出裁剪面仍可见" | ✅/验证 |
| 2c | 与 L2 正交性验证：forceVisible 块进候选集后，块内实例仍走 GPU 视锥剔除（不误免） | 📋 随 L2 验证 |

### 阶段 3：L2a 实例矩阵上传

| 步骤 | 内容 | 状态 |
|:--|:--|:--:|
| 3a | 块内实例矩阵 + 实例包围球半径 → StructuredBuffer（静态加载一次上传，`StaticEntityPersistentBuffer` 模式） | 📋 待做 |
| 3b | 实例半径 = 网格 localBounds × 实例缩放，MapSceneConverter 侧预计算写入 | 📋 待做 |

### 阶段 4：L2b GPU 实例剔除

| 步骤 | 内容 | 状态 |
|:--|:--|:--:|
| 4a | Compute 剔除 pass：每实例一个 thread，包围球 vs 视锥（仅可见块上传） | 📋 待做 |
| 4b | AppendBuffer / Atomic 计数 → IndirectArgsBuffer | 📋 待做 |
| 4c | 可选：距离 LOD 选择（tree 近/远） | 📋 可选 |

### 阶段 5：L2c 间接绘制

| 步骤 | 内容 | 状态 |
|:--|:--|:--:|
| 5a | `ExecuteIndirect` 渲染路径（树/广告牌 ~11787 实例）——**命令数 = 材质段数**（每桶一次，`MaxCommandCount=子网格段数` 一次提交多段） | ✅ 已落地（2026-08-08） |
| 5b | 相机静止缓存：ViewProj 未变时跳过 compute，复用剔除结果 | 📋 待做 |
| 5c | 阈值启用：小图（In 510×510）按块数/实例数阈值走传统路径 | 📋 待做 |

**定案（2026-08-08）——实例=实体、桶=材质段**：
- **实例（剔除票）= 实体级**：1 实体 = 1 包围球 = 1 次剔除，**不按子网格拆分**（对齐 UE GPUScene 实例级剔除）。此前实现把子网格当实例（world 复制 N 份、剔除 N 次、ExecuteIndirect 1800 条）为错误，已修正
- **桶（绘制命令）= 材质段级**：同材质多子网格区间并入一桶（对齐 Unity BRG materialID 批次），材质切换才分桶
- **CS 计数**：实体可见后对其拥有的每个材质段分别原子计数（分段 Append），每段 InstanceCount = 含该材质的可见实体数

**已知缺陷（2026-08-08 记录）——实体材质段桶数量固定上限截断**：

现象：City 场景 `truncatedRefs=253` 恒定（FrameSync 日志），253 个 5 槽实体（mapChip06 等）的**第 5 个材质段桶引用被截断** → 被截断桶 `InstanceCount` 恒 0（空桶，RenderDoc 见 `IndirectDrawIndexed(<N,0>)`）→ 对应材质段不绘制 → 运行时错乱。

根因：`GPUInstanceData.bucketIndices[4]` + `kMaxBucketsPerEntity=4`（C++/HLSL 双端硬编码）**假设单实体材质段桶数 ≤4**——但材质桶数来自场景 JSON `materials[]` 长度（资产数据，任意），无约束保证 ≤4。固定数组承载不确定长度的桶归属，属设计缺陷。

**方案 B（2026-08-08 定案）——桶归属移出实体结构，扁平映射表表达**：

| 设计点 | 内容 |
|:--|:--|
| 原则 | **无固定上限**：实体材质段桶数 = Σ 实体槽位（动态），对齐 UE GPUScene material 分段思想 |
| `GPUInstanceData` 新布局 | 只保留 `world/radius/bucketOffset/bucketCount`（96B→约 80B 对齐），移除 `bucketIndices[4]` |
| `EntityBucketMap`（新缓冲） | 扁平 `{uint32 bucketIdx}[]`（按实体分段连续存储），长度 = Σ 实体槽位，GPU SRV |
| CS 读取 | 实体可见后 `for (m = bucketOffset; m < bucketOffset + bucketCount; ++m) 对 gBucketMap[m] 桶计数`——替代固定数组遍历 |
| FrameSync 生成 | 构建扁平映射表：实体首遇写 `bucketOffset = map.size()`，每新桶 `map.push_back(bucketIdx)`、`bucketCount++` |
| 兼容 | 剔除仍按实体（1 票）；桶偏移表/Append 分段/ExecuteIndirect 消费不变 |

状态：📋 方案 B 逐步推进中（2026-08-08 起），详见 `Docs/todos/InstanceCulling_L2c_Todo.md` §六。

### 阶段 6：L3 遮挡（📋 2026-08-12 提前，原远期）

| 步骤 | 内容 | 状态 |
|:--|:--|:--:|
| 6a | HZB 深度遮挡（**双时域消费**：本帧 SSR/接触阴影 + 下一帧遮挡剔除） | ✅ 已接入（2026-08-12）：HZB 生成 + 下一帧 PrePass 遮挡剔除（朴素两趟——P0 近平面跳过/P1 膨胀/fovea/三档联动已注释停用，见 TwoPassHZB.md）；本帧 SSR 消费已实现（RenderPhase::SSR + 反射图 + 全场景合成）——接触阴影未实现（阴影剔除方案见 ShadowCullingGPUDriven.md） |
| 6b | 保守化 PVS 重做（只剔小物体、排除地块，上版教训见 `BugFix_Editor_StaticDirtyState_And_OutlinerIssues.md` §五） | 🔭 远期 |

**HZB 双时域消费（2026-08-12 定案）**——HZB 不是"只给下一帧用"的单消费语义：

| 消费者 | 用哪帧的 HZB | 原因 |
|:--|:--|:--|
| 遮挡剔除（PrePass dispatch） | **上一帧** | 剔除在渲染命令录制阶段，本帧深度尚未生成——只能用旧的 |
| SSR（RenderPhase::SSR 独立阶段） | **本帧** | 效果执行时本帧深度已完整，HZB 生成完毕即可用（2026-08-12 已实现：HZB 层级步进 → 半分辨率反射图 + CompositePS 全场景菲涅尔合成） |
| 接触阴影（未实现） | **本帧** | 预留——对齐 SSR 的消费语义 |

**阶段流水线定案（2026-08-12）**——`RenderPhase::HZB_Build` 单开阶段（枚举已按 Tick 顺序重排并新增，FrameDriver.cpp Tick 已插入提交点）：

```
PrePass      遮挡剔除（消费【上一帧】HZB）+ 清屏 + G-buffer
  → Opaque   渲染不透明物体 → 生成当前帧深度图
  → HZB_Build  构建 HZB（消费本帧深度图 → mip 链）【新增阶段，串行保证执行顺序】
  → DynamicAOcclusion  SSAO（直接采样深度，不依赖 HZB）
  → Lighting  延迟光照（SSR/接触阴影消费【本帧】HZB）
  → Billboard → Transparent → PostProcess → FSR3_Upscale → UI
```

**mip 规则**：

| 项 | 值 |
|:--|:--|
| mip0 尺寸 | = 深度缓冲尺寸（视口/窗口，随 OnResize 重建；1280×720 初始） |
| 每级尺寸 | `W_i = max(1, ceil(W/2^i))`，H 同理（非 POT 允许） |
| 层级极限 | `floor(log2(max(W,H))) + 1`，到 1×1 为止（1280×720 = 11 级） |
| 降采样归约 | D3D 深度 0=近/1=远 → 每级取 **min（最近面）**，保守不误剔 |
| 格式/显存 | R32_FLOAT 全链 ≈ 4/3 × mip0 ≈ ~4.9MB（1280×720） |

**目录归属（2026-08-12 修正）**——HZB 是**屏幕空间基础数据**（同 SSAO/AO 一类），**不属于剔除层**，归 `Effects` 目录（对齐 `Effects/AO/`）；剔除层只做可见性判定：

```
Renderer/Effects/Hzb/  # ✅ 已实施（2026-08-12）：HzbManager.h/.cpp + HzbRenderer.h/.cpp
  #   HzbManager：AO 管理器骨架（生命周期/OnResize/资源持有，无开关——HZB 是基础数据默认生成）
  #   HzbRenderer：CS 降采样（Shaders/HzbBuild.cs.hlsl，自写逐级 2×2 min，不用 SPD）
CullingLayer/          # 剔除层：CullingRenderer / CullingResourceManager / CullingSystem / SpatialHashGrid / CulledSet
```

**注册**：`EditorHzbBuildSystem`（RenderPhase::HZB_Build，Opaque 之后、Lighting 之前，Editor.cpp）——资源守卫在 Acquire 前完成（规则 26），内部对称屏障 COMMON→SRV/UAV→COMMON（规则 10），无消费者时为空提交零成本。

**不使用 FidelityFX SPD**（2026-08-12 定案）：FidelityFX 仅用于 FSR（ThirdParty 内，引擎/Editor 零引用）——CORE 不引入无关内容；HZB 的 mip 降采样**自写 CS**（逐级 2×2 降采样，代码量小，无需 wave intrinsics）。

**保守化原则（2026-08-12 修订）**：HZB 遮挡测试**所有物体（含大物体）都参与**——保守性由 **max 归约**保证（HZB 存最远面；足迹含天空/远值 1.0 → `objNear < 1.0` 不剔；只有足迹内全部为更近遮挡面才剔）。**不设"只剔小物体"像素阈值**（2026-08-12 移除：16px 阈值导致中近距离物体全跳过、HZB 近乎失效——静态推演 + RenderDoc 实证）。物体最近深度用包围球前表面（`centerDepth - radius*0.5/clipPos.w`，保守折减，避免大物体中心被挡但边缘可见被误剔）。时域滞后（上一帧 HZB）由 max 归约 + 前表面深度近似吸收，宁可漏剔不可错剔。§6b 保守化 PVS（只剔小物体/排除地块）仍是**离线 PVS** 的独立原则，与实时 HZB 无冲突。

**帧数据错位说明（2026-08-12）**：Immediate 相机 vs LateUpdate 剔除视锥 vs Render dispatch 天然错开一帧——对 HZB **不是问题**（时域设计容忍滞后 + 保守膨胀吸收错位）；实例剔除数据竞态（CullData/桶偏移每帧覆写固定 UPLOAD）是**另一码事**，需 RingBuffer + 帧记号方案单独修。

---

## 六、关键权衡

- **收益**：CPU 逐实例剔除从 15489 次 → 5 块；每帧实例矩阵上传省掉（静态一次上传）；Builder 并行压力下降（RendererDataDriven §4.1b 讨论点）
- **代价**：compute pass 调度 + 间接绘制调试复杂度（无 CPU 可见的实例计数）
- **边界**：动态物体不进 L2（每帧矩阵重建，上传反而贵）；建筑状态切换不进 L2（`_d` 切换走渲染项替换）；小图（In 510×510）L2 收益低，可按块数/实例数阈值启用

---

## 七、自适应 HZB 切换策略（2026-08-12 记录，§7.2-7.4 已实施）

> 定案：这是一套**安全、稳健且自适应**的剔除策略——让 HZB 在擅长的场景（静态、缓慢运动）发挥最大效能，在不擅长的场景（剧烈旋转/俯仰）自动"退让"避免闪烁。**2026-08-12 注释停用 → 2026-08-13 已移除**（P0 近平面跳过 / P1 俯仰膨胀 / §7.3 相机联动三档 / fovea 屏幕感知——实测误判率高，仅保留朴素两趟 HZB，见 `TwoPassHZB.md`）；§7.1 物体分类标签（Static/Dynamic/Tiny）待后续按需实施。增强字段与分档逻辑已从 `CullParams`（176B ↔ HLSL 同步）与 `DispatchCulling` 签名删除，文档保留供恢复参考。

### 7.1 物体分类（数据层面：剔除策略标签，加载时确定、运行时不变）

| 标签 | 包含对象 | 剔除方式 |
|:--|:--|:--|
| `Static` | 建筑、地形、大型静态残骸 | HZB + 视锥体 |
| `Dynamic` | 角色、载具、飘移碎片 | 视锥体 + 距离 |
| `Tiny` | 粒子、弹壳、小型碎片 | 仅距离剔除或不剔除 |

### 7.2 剔除流水线（按帧阶段）

```
帧开始
  → [1. CPU 粗筛]（可选）：距离剔除（> MaxRenderDistance 丢弃）+ 空间哈希粗块剔除（相机背后整块丢弃）
  → [2. PrePass 阶段]：读上一帧 HZB（HZB_Previous）
       ├─ Static：①视锥体裁剪 ②HZB 遮挡剔除
       └─ Dynamic/Tiny：①视锥体裁剪 ②距离剔除（Dynamic 更宽松阈值）
       → 生成"可见实例列表"供 Opaque 使用
  → [3. Opaque 阶段]：渲染所有可见实例 → 生成当前帧深度图
  → [4. HZB_Build 阶段]（DynamicAOcclusion 之前）：当前帧深度图 → HZB_Current（下一帧作 HZB_Previous）
  → [5. Lighting 阶段]：HZB_Current 加速 SSR/接触阴影
  → [6. 后续阶段]：Transparent / Billboard / PostProcess / UI
```

### 7.3 动态 HZB 切换（解决俯仰角问题：按相机运动状态决定是否信任上一帧 HZB）

> **❌ 2026-08-13 已移除（文档保留供恢复参考）**：三档判定在 Editor.cpp 已删除，`gHzbMode`/`gPitchExpand`/
> `gHzbBias` 字段从 `CullParams` 与 `DispatchCulling` 签名移除。
> 原因（两趟 HZB 定案，见 `TwoPassHZB.md`）：早期 HZB = 上一帧 HZB，本帧剔除直接消费；
> **新进入视野物体在其屏幕区域无深度记录 → HZB 采样 1.0（远）→ ObjNear < 1.0 不剔**，
> 天然安全——无需相机运动退让（原先的"俯仰误剔"认知已修正，PrePassDepth 亦不需要）。
> 保留内容：朴素遮挡测试（AABB 投影 → mip → 4 采样比较）+ HLSL 写死常量 `kHZBNearPlane = 0.5f`
> （近裁剪面防御，对齐 Camera.h NearPlane）。若未来需要退让策略（如 TAA 抖动/超采样引入
> PrePassDepth 可选项）可按本文档恢复字段与分档逻辑。
> P1 俯仰膨胀 / fovea 屏幕感知 / P0 近平面跳过（AdaptiveFarPlane.md）同批**已移除**：
> HLSL 遮挡测试块仅保留朴素 AABB 投影 → mip → 4 采样比较，Editor.cpp 调用点已精简。

**核心指标**（每帧 CPU 计算）：
- `ΔAngle`：本帧与上一帧相机朝向夹角（度）
- `ΔPosition`：本帧与上一帧相机位置距离

**切换逻辑**（`HZB_Mode` 全局 Uniform，CPU 上传、CS 读取）：

| 条件 | HZB_Mode | 策略 | 原因 |
|:--|:--:|:--|:--|
| `ΔAngle < 15°` 且 `ΔPosition < 5m` | 0 正常 | 正常使用 HZB（z_min > maxDepth） | 运动平缓，上一帧深度可信 |
| `15° ≤ ΔAngle < 45°` | 1 保守 | 强制低 Mip（如 Mip3）+ 保守偏置（z_min + 0.01 > maxDepth） | 中度旋转，低 Mip 稳定免闪烁 |
| `ΔAngle ≥ 45°` 或 `ΔPosition ≥ 20m` | 2 禁用 | 全部标记可见（occluded=false），仅视锥 + 距离 | 剧烈运动，上一帧深度完全失效 |

```hlsl
// 伪代码（HZB_Mode 由 CPU 每帧计算上传）
if (HZB_Mode == 0) { bool occluded = (z_min > maxDepth); }
else if (HZB_Mode == 1) { float bias = 0.01; bool occluded = (z_min + bias > maxDepth_Mip3); }
else { bool occluded = false; }
```

### 7.4 近平面安全处理（已实现 P0 基础，规划完善）

```
如果 包围盒与近平面相交：
    跳过 HZB 遮挡测试，直接标记"可见"（避免物体靠近相机时被误剔/穿模消失）
```

逻辑放在 HZB 比较之前作为第一道过滤——当前已实现 `cp.w <= gNearPlane` 跳过（2026-08-12 P0）。

### 7.5 策略效果

| 场景 | HZB 模式 | 剔除精度 | 画面稳定性 |
|:--|:--|:--|:--|
| 相机静止/缓慢平移 | 正常 HZB | 高（剔除大量遮挡物） | 完美 |
| 相机缓慢俯仰/偏航 | 保守 HZB（低 Mip） | 中（剔除部分遮挡物） | 稳定，无闪烁 |
| 相机快速旋转/瞬移 | 禁用 HZB | 低（仅视锥体剔除） | 稳定，性能略降 |
| 近平面相交物体 | 强制可见 | 不剔除 | 稳定，无"穿模消失" |

### 7.6 实施建议（分版推进）

1. **第一版**：先实现"正常模式 + 禁用模式"二档，阈值 `ΔAngle = 30°`——解决 90% 俯仰角闪烁；
2. **第二版**：加保守模式（低 Mip）中间档，优化中度旋转剔除效率；
3. **第三版**：完善近平面安全处理，确保物体靠近相机不消失。

> 参照：UE5 / Unity HDRP 已验证同类策略——静态场景高效剔除 + 自适应切换保证动态视角稳定。

---

## 八、参考

- 大型引擎分层：UE（World Partition + GPUScene + Nanite cluster 剔除）、Unity（BRG + SRP Batcher）、id Tech（PVS 只覆盖静态）、CryEngine（VisArea+Portal 静态专用）——详见 `../culling/OctreeCullingAndRaycaster.md` §6、`RendererDataDriven.md`
- 历史：`GPU-Drive.md` 早期版本为 AI 对话记录（程序化生成模式对比），已由本文取代
