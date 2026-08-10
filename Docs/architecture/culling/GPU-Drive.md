# GPU Driven 剔除分层蓝图（2026-08-06 定稿）

> 日期：2026-08-06
> 状态：📋 设计蓝图（分层定案，实现待排期）
> 关联：`08_MapScenePipeline.md` §八（区块化聚合，500 单位块 + 实例矩阵数组）、
> `../culling/OctreeCullingAndRaycaster.md`（空间哈希 + CulledSet）、
> `BugFix_Editor_StaticDirtyState_And_OutlinerIssues.md` §六（CPU 剔除定稿）、
> `BillboardSystemArchitecture.md`（公告牌/树 = 交叉 quad）
>
> 背景：City4 场景 15489 个 ECS 实体（tree 8404 + tree5/tree2 3383 + 广告牌 2798 + 建筑 ~900），
> 94% 是树与广告牌；树本质是"略微旋转的交叉 quad"（~24 顶点/14 三角形），公告牌表达成立，
> 是实例化 + GPU 剔除的理想输入。

---

## 零、核心认知澄清（先纠偏，再谈层级）

| 说法 | 实际 |
|:--|:--|
| "实体数量级减少了" | ✅ **ECS 实体数确实减少**：15489 → 5 块（BlockComponent 聚合，-99.97%，`08_MapScenePipeline.md` §8.4 已定案） |
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
L3 遮挡剔除（🔭 远期可选）
  HZB 深度遮挡 / 保守化 PVS（只剔确定被遮挡的小物体，排除地块——上版 PVS 失败教训见
  `BugFix_Editor_StaticDirtyState_And_OutlinerIssues.md` §五）
```

| 层 | 粒度 | 内容 | 执行者 | 频率 | 状态 |
|:--|:--|:--|:--|:--|:--:|
| L1 | 块实体（≤5） | 空间哈希/视锥/cullDistance → CulledSet | CPU | 每帧（静止缓存） | ✅ |
| L2 | 块内实例（~4611/块） | 矩阵上传（静态一次）→ compute 剔除 → 间接绘制 | GPU | 每帧（仅可见块） | ❌ |
| L3 | 实例 | HZB 深度遮挡 / 保守化 PVS | GPU/离线 | 每帧/场景级 | 🔭 |

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

## 三、L2 数据流（待实现）

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
| `OctreeSystem`（空间哈希） | 原样保留，管 L1 块级粗筛（"实体"→"块实体"，§8.5 已定案） |
| `CullingSystem` / `CulledSet` | 仍是唯一入口：命中块 → L2 上传/剔除；未命中 → GPU 不碰 |
| `RenderSlotCache` | 分桶逻辑不变，块实体照常入桶（shaderType 路由） |
| `OpaqueRenderItemBuilder` 等 | 移除逐实例视锥测试（`Frustum::Contains`），只消费块级结果 |
| PVS | 降级为远期 L3 可选项，必须保守化重做（只剔小物体，排除地块） |

### 4.1 查询单元与数据组织单元对齐（2026-08-06，`08_MapScenePipeline.md` §8.7）

> 审视结论（用户定案）：**空间哈希查询单元 ≠ 块数据组织单元，需要解耦 + 块可配置**。
> 现状代码三套粒度脱节：空间哈希格子（`OctreeSystem::m_cellSize=250` 硬编码）、
> BlockComponent 集群（Phase C 500 单位）、资产侧聚合块（§8.5 定案 500 单位）——三者各自固定。

| 单元 | 角色 | 归属 | 配置方式 |
|:--|:--|:--|:--|
| 空间哈希格子（查询单元） | 视锥剪枝粒度，格子级跳过 | 引擎（`OctreeSystem::m_cellSize`） | 引擎默认 250，可随块联动 |
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
| UE `bAlwaysVisible`（跳过视锥测试，遮挡查询仍可剔除） | `ComputeViewVisibility()` 视锥循环内 | `BlockComponent.forceVisible` → `OctreeSystem::m_forceVisibleEntities`（查询时直通候选集，✅ 已实现） |
| UE HLOD Cluster 大包围体（远距合并代理） | World Partition 流式后、视锥剔除前 | `clusterBounds` + `FrustumCullAABB` 15% 扩展（✅ 已实现） |
| UE 远平面 / cull distance volume / 自适应远平面 | 相机矩阵构建时 | `AdaptiveFarPlane.md`：CullFarPlane=1000 宽剔除平面（✅ 已实现） |

```
集群 forceVisible（对抗远裁剪）→ 块进候选集（粗筛豁免，不被远平面裁掉）
        ↓ 叠加
L2 GPU 实例剔除（对抗视角方向）→ 块内实例逐个视锥，背面/视锥外实例仍被剔掉
```

**适用对象**：山、远距建筑群、地形边界等"内容太大，超出裁剪面仍应可见"的内容（用户定案：
"山不应被远裁剔除——地形边界内容不适用远近裁剪"）。小块树/广告牌**不需要** forceVisible——它们靠块级粗筛 + L2 GPU 剔除即可。

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

### 阶段 1：块级粗筛落地（L1 补齐，2026-08-04 已有基础）

| 步骤 | 内容 | 状态 |
|:--|:--|:--:|
| 1a | 块实体（持实例矩阵数组）入空间哈希：`OctreeSystem::AddEntity` 存**块实体**而非实例实体（§8.5 意图落地） | 📋 待做 |
| 1b | `RenderSlotCache` 桶存**块条目**（Entry 指向块实体），Builder 消费块再取矩阵 | 📋 待做 |
| 1c | 验证块级 CulledSet（City4 ≤5 块）→ 桶 → Builder 链路 | 📋 待做 |

### 阶段 2：集群豁免接入（§4.2，纯剔除豁免器）

| 步骤 | 内容 | 状态 |
|:--|:--|:--:|
| 2a | `forceVisible` 直通候选集：✅ **已实现**（`OctreeSystem::m_forceVisibleEntities`）——验证山/远距建筑群走豁免 | ✅/验证 |
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

### 阶段 6：L3 遮挡（远期可选）

| 步骤 | 内容 | 状态 |
|:--|:--|:--:|
| 6a | HZB 深度遮挡（静态世界深度缓冲，剔除被挡实例） | 🔭 远期 |
| 6b | 保守化 PVS 重做（只剔小物体、排除地块，上版教训见 `BugFix_Editor_StaticDirtyState_And_OutlinerIssues.md` §五） | 🔭 远期 |

---

## 六、关键权衡

- **收益**：CPU 逐实例剔除从 15489 次 → 5 块；每帧实例矩阵上传省掉（静态一次上传）；Builder 并行压力下降（RendererDataDriven §4.1b 讨论点）
- **代价**：compute pass 调度 + 间接绘制调试复杂度（无 CPU 可见的实例计数）
- **边界**：动态物体不进 L2（每帧矩阵重建，上传反而贵）；建筑状态切换不进 L2（`_d` 切换走渲染项替换）；小图（In 510×510）L2 收益低，可按块数/实例数阈值启用

---

## 七、参考

- 大型引擎分层：UE（World Partition + GPUScene + Nanite cluster 剔除）、Unity（BRG + SRP Batcher）、id Tech（PVS 只覆盖静态）、CryEngine（VisArea+Portal 静态专用）——详见 `../culling/OctreeCullingAndRaycaster.md` §6、`RendererDataDriven.md`
- 历史：`GPU-Drive.md` 早期版本为 AI 对话记录（程序化生成模式对比），已由本文取代
