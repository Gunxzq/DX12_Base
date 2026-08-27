# 实例剔除系统架构（Instance Culling System）

> 日期：2026-08-10
> 状态：⚠️ 部分过时（管道/生产者边界框架仍有效；桶接口 `GetBucketMapSRV`/`GetBucketOffsets` 等
> 随 2026-08-18 无桶流程废弃——现行中间层接口 = `SetCullData` + `SetSegmentTables` 两张段表）
> **阶段定位**：S1（空间粗筛）/ S3（CPU 构建器）/ S4（拼接上传）/ S5（GPU 剔除 CS）总体架构——
> 剔除系统 = 管道/生产者，渲染管线 = 消费者。阶段表见本目录 `README.md`
> 关联：`../rendering/GPU-Drive.md`（L1/L2/L3 分层蓝图）、`OctreeCullingAndRaycaster.md`（空间哈希 + CulledSet + 双轨制）、
> `../rendering/RendererDataDriven.md`（Builder 桶模式 + FrameSync 统一上传）、`../rendering/LOD.md`（LOD 系统）、
> `../core/EventSystemAndDataLayer.md`（SharedDataStore 中间层先例——本系统三层抽象的设计参照）
>
> 背景：实例剔除系统经多轮拆分/合并/修正（2026-08-08 ~ 08-10），已完成大型引擎标准形态的实例剔除链路
> （L2c Todo §7.6 调研确认）。本文档定案：剔除系统 = 管道/生产者，渲染管线 = 消费者，
> 边界划分对齐 UE GPUScene / Unity BRG / Nanite 官方文档验证。

---

## 一、核心认知：剔除系统是"管道/生产者"

**剔除系统 = 资源的生产者（管道）**：消费 ECS 实体 → 空间划分 → 视锥粗筛 → 统一上传 → GPU 剔除 →
**产出剔除结果**（AppendBuffer 存活实例 / IndirectArgs 桶实例数）。它不负责"画"，只负责"决定画什么"。

**渲染管线 = 消费者**：ExecuteIndirect 绘制、材质槽、LOD 选择均属渲染管线。
**块展开（块→成员）归剔除层**（2026-08-10 定案，见 `GPU-Drive.md §4.3`）——区块生成/存储/
展开都是空间哈希（剔除层）的任务。
两者通过**中间层接口**交换数据，渲染系统不感知剔除内部实现。

```
┌─────────────────────────────────────────────────────────────────────┐
│                        剔除系统（管道/生产者）                          │
│                                                                     │
│  ① 空间哈希粗块划分   ② 预测视锥粗块候选集   ③ 块展开   ④ 统一上传  ⑤ 剔除 CS │
│  (SpatialHashGrid)   (QueryFrustum)     (块→成员)  (FrameSync) (Dispatch)│
│                                                                     │
│         ▼ 中间层接口：CulledSet / GetInstanceSRV / GetBucketMapSRV    │
│            GetBufferAddress / GetBucketOffsets / GetInstanceCount     │
├─────────────────────────────────────────────────────────────────────┤
│                        渲染管线（消费者）                              │
│  Builder 渲染项（消费成员实体）→ ExecuteIndirect 绘制                   │
│  LOD 选择（PickLOD）· 材质槽（RenderSlotCache/Builder）                │
└─────────────────────────────────────────────────────────────────────┘
```

---

## 二、剔除全流程（5 步 + 补充环节）

### 主流程（与实现对照）

> **2026-08-27 注**：③④⑤ 与下方补充 A/B/D 描述的是 **2026-08-18 之前的桶流程**（Builder 消费桶 +
> 实例数据/桶偏移表统一上传 + 每桶 `ExecuteIndirect`），现已被**无桶流程**取代：Builder 产出
> `CullData[]`（逐实例 1:1 不合批，`groupId` = 段内命令索引）+ `IndirectCommand[]`（每 BatchKey 一条
> 完整命令）→ FrameSync 拼接上传（`SetCullData` + `SetSegmentTables` 两张段表）→ CS 只原子累加
> InstanceCount → **一次 `ExecuteIndirect`**。①②（空间哈希粗筛）仍为现行。

| # | 环节 | 实现 | 阶段 | 状态 |
|:--|:--|:--|:--|:--|
| ① | 空间哈希粗块划分（块实体入格——clusterBounds 大包围盒，双轨制静态 Build） | `SpatialHashGrid::Build` | PreCulling | ✅ |
| ② | 预测视锥剔除 → 粗块候选集（相机静止复用 coarse） | `QueryFrustum` → `m_octreeCoarse` | PreCulling | ✅ |
| ③ | 构建器从候选集得渲染项（块展开在渲染管线侧完成，见补充 A） | `RenderSlotCache::Dispatch` + `OpaqueRenderItemBuilder` | PreRender | ✅（块展开归属见补充 A） |
| ④ | 统一上传阶段（实例数据 + 桶偏移表） | FrameSync `SetFlatInstances` + 立即回调 `Upload` | FrameSync | ✅ |
| ⑤ | 剔除 CS 执行（视锥球测试 → AppendBuffer/IndirectArgs） | `DispatchCulling` | Render（PrePass） | ✅ |

### 补充环节（职责归属定案）

- **补充 A · 块展开**：粗筛候选是块条目（CulledSet 简洁引用 `{blockIndex, clusterBounds, sceneId}`），
  Builder 消费的是块内成员展开——**2026-08-11 终版：块展开缓存归渲染管线侧**
  （`RenderSlotCache::m_blockExpanded`，Rebuild `view<BlockComponent>` 构建驻留、Dispatch 每帧引用——
  对齐 UE FMeshDrawCommand CachedMeshDrawCommands：AddToScene 预构建、每帧只选命令）。
  块 = ECS 组件（区块组件，持 memberEntities + clusterBounds），**不预展开**（CulledSet 只引用
  blockIndex，避免每帧成员拷贝浪费——Builder 并行消费桶不读 CulledSet）。详见 `GPU-Drive.md §4.3`。
- **补充 B · 剔除结果消费**：`gIndirectArgs`（每桶 InstanceCount）→ 每桶 `ExecuteIndirect`（OpaqueRenderer `DrawInstancedGBufferIndirect`）→ VS 从 `gAppendBuffer` 读存活实例。**归属渲染管线**（绘制阶段），非剔除系统。
- **补充 C · 动态实体并行路径**：机体/子弹/特效 <50 → **CPU 视锥 + 每帧上传**（GPU-Drive.md 定案，不进 L2）。架构预留 GPU 化通道（见 §五）。
- **补充 D · 辅助环节**：readback 可见数验证（`ReadbackVisibleCount` + Verify 日志）、剔除资源生命周期（SRV/段地址/AppendBuffer 扩容）、相机静止缓存（②复用 coarse）。

---

## 三、边界划分定案

| 子系统 | 内容 | 归属 |
|:--|:--|:--|
| **剔除系统（管道/生产者）** | 空间哈希粗筛（块实体入格）→ 预测视锥粗块候选 → 统一上传 → 剔除 CS → 剔除结果输出；剔除用 SRV/UAV/缓冲（AppendBuffer/IndirectArgs/CullParams/实例 SRV/bucketMap SRV/readback/桶偏移表）；相机复用缓存（复用 L1 coarse） | `Engine/Renderer/Culling/`（规划） |
| **渲染管线（消费者）** | 块展开（A，块→成员 Entry，RenderSlotCache::m_blockExpanded）、ExecuteIndirect 绘制（B）、材质槽、LOD 选择 | `Pipeline/` + Builder |
| **中间层接口** | CulledSet（块条目简洁引用 {blockIndex, clusterBounds, sceneId}）/ GetInstanceSRV / GetBucketMapSRV / GetBufferAddress / GetBucketOffsets / GetInstanceCount——渲染系统不感知剔除内部（⚠️ `GetBucketMapSRV`/`GetBucketOffsets` 已废弃，2026-08-18 无桶流程 → `SetCullData` + `SetSegmentTables` 两张段表） | — |

**关键原则**：剔除系统是"管道/生产者"，只决定"画什么"；渲染管线消费结果并绘制。两者经中间层接口解耦
（本次会话 SRV 生命周期跨帧断裂根因：数据扁平化与 dispatch 生命周期耦合——中间层化后不可能再犯）。
**块展开归属**（2026-08-11 终版，`GPU-Drive.md §4.3`）：块 = ECS 组件（区块组件，持 memberEntities +
clusterBounds）；块展开缓存归**渲染管线侧**（`RenderSlotCache::m_blockExpanded`，Rebuild
`view<BlockComponent>` 构建驻留、Dispatch 每帧引用——对齐 UE FMeshDrawCommand CachedMeshDrawCommands）；
**不预展开**（CulledSet 块条目只引用 blockIndex，成员 ID 驻留块组件，避免每帧拷贝浪费）。
历史 2026-08-10 定案"块展开归剔除层 + 查询展开成员"因预展开拷贝浪费废弃。

---

## 四、LOD 归属定案（2026-08-10）

### 定案：LOD 选择归属构建器（渲染管线消费者侧），不跟随 CS 输出

| 项 | 归属 | 理由 |
|:--|:--|:--|
| LOD 数据存储（LODMesh/LODSystem） | 资产/数据层（Core） | 与剔除无关 |
| **LOD 选择（PickLOD）** | **构建器**（OpaqueRenderItemBuilder，精确视锥筛选后顺位） | 查询行为 + 产生渲染项（几何句柄进桶键 `{geometry, materialIdx}`）→ 天然在 Builder |

### 为什么"LOD 跟随 CS 输出"不可行（排除方向）

1. **离散 LOD 句柄切换**：`LODMesh` = 多个独立几何句柄，切换后几何数据完全不同；
2. CS 选 LOD 需实例携带阈值表（数据膨胀）+ GPU→CPU 回读选择结果（延迟 1 帧）或 CPU 预测；
3. **渲染项几何句柄由 CPU 侧 LOD 决定**——若 CS 输出 LOD 级别，桶键 {geometry, material} 依赖 GPU 结果
   → 桶结构动态化，Builder/渲染管线架构被打破；
4. **大型引擎先例**：UE 传统 LOD（InitViews CPU 选句柄）、Unity LODGroup（Culling CPU 选级）——
   **均不在 GPU 剔除 CS 中选离散句柄**。

### Nanite 特例说明（未来方向记录，非当前参照）

**Nanite 的 GPU LOD 是 cluster 虚拟化**：同一网格的细节密度（cluster 层次结构），GPU 剔除遍历 cluster
层次时自然选级，绘制始终同一实例/材质——**不是离散句柄切换**。这是 GPU 内 LOD 的唯一大型引擎案例，
但依赖虚拟化几何（cluster BVH），与本项目离散 LODMesh 模型不兼容。

> **记录（2026-08-10）**：短时间无法演变为 Nanite 模式（虚拟化几何 + cluster 剔除），虽可能是剔除的
> 最高效最终形态。当前定案：离散 LOD 选择留在构建器；未来若引入虚拟化几何（cluster LOD），
> 剔除系统自然承载 LOD 选择（cluster 层在剔除管线内部），届时架构升级为 Nanite 式。

---

## 五、动态实体路径（预留 GPU 化通道）

| 现状（GPU-Drive.md 定案） | 说明 |
|:--|:--|
| <50 动态实体 → CPU 视锥 + 每帧上传，不进 L2 | 数量少，每帧矩阵重建上传反而贵（GPU-Drive.md:252） |

**架构预留**：剔除数据层（CullingDataStore 规划）支持"静态块 + 动态实体均可写入 InstanceData 数组"——
未来动态实体数量增长时无缝并入 L2（统一上传阶段把动态矩阵并入 allInstances）。大型引擎（UE
UpdatePrimitiveTransform / Unity BRG dynamic batch）均支持动态实体进 GPU 剔除（每帧更新数据），
本项目 CPU 视锥是数量权衡而非架构限制。

---

## 六、三层中间层抽象（规划，代码拆分待排期）

**目标**：把剔除系统拆为"数据层 / 资源层 / 执行层"（对齐事件系统 SharedDataStore 中间层范式），
各自可独立测试与演进。**目录骨架当前不建立**（2026-08-10 用户定案），本文档先行记录设计。

```
Engine/Renderer/Culling/（规划）
├── SpatialHashGrid.h/.cpp         ← 空间哈希粗筛（原 OctreeSystem 改名，粗筛层，双轨制 Build/AddEntity）
├── CullingDataStore.h/.cpp        ← 数据层：InstanceData/bucketMap/桶偏移表/段地址（数据扁平化与 GPU 解耦）
├── CullingResourceManager.h/.cpp  ← 资源层：AppendBuffer/IndirectArgs/CullParams/readback 生命周期
└── InstanceCullingRenderer.h/.cpp ← 执行层：CS dispatch/PSO/根签名/readback 统计（对齐 IRenderer 契约）
```

| 层 | 职责 | 收益 |
|:--|:--|:--|
| 数据层 | 数据扁平化 + 上传（不碰 PSO/UAV） | 解除"数据扁平化 ↔ dispatch 生命周期"耦合（本次根因） |
| 资源层 | 资源生命周期 + 扩容（对齐 GpuResourceManager 协作模式，规则 11） | SRV/段/扩容集中管理 |
| 执行层 | CS dispatch + PSO/根签名（对齐 IRenderer 契约） | 遵循渲染器模式（规则 24） |

### 6.1 剔除层对外接口定案（2026-08-10 讨论定案）

**核心认知**：**剔除层 = 门面（Facade）**——包装剔除系统中的模块（SpatialHashGrid（原 OctreeSystem）/ CullingDataStore /
CullingResourceManager / InstanceCullingRenderer）及其方法，对外统一暴露。渲染管线（构建器/渲染器）
只与剔除层交互，不感知内部模块。

| 面 | 接口 | 定案依据 |
|:--|:--|:--|
| **视锥输入** | `void SetPredictedFrustum(const Frustum &frustum);`（帧首注入） | 输入方式 B：剔除系统有视锥体就够；**相机预测不归剔除系统**（TAA 等多处使用，留在相机系统） |
| **ECS 数据连接** | `void BindWorld(ECS::World &world);`（内部 GetRegistry 遍历） | 方案 C（注入 + 内部遍历）；需处理**场景 ID**：编辑器端多 World 对应多场景——每 World 映射 sceneId |
| **system 注册** | `void RegisterSystems(FrameDriver &driver);` + `static const char *GetTaskName(...)` | 剔除层自注册；**暴露稳定唯一 task name 供构建器 `DependsOn`**（防同名混淆，串行保证） |
| **上传阶段** | `void Upload(FrameResourceManager *frameResMgr);`（立即回调后）/ `SetFlatInstances(...)`（FrameSync）/ `EndFrame(fence)`（帧末） | 时序契约：**上传先于消费**（立即回调上传 → 本帧 dispatch 消费，对齐 FrameDriver 时序：Render 读上一帧 FrameSync 数据） |
| **消费输出** | `GetCoarseSet()` / `GetInstanceSRV()` / `GetBucketMapSRV()` / `GetBufferAddress()` / `GetBucketOffsets()` / `GetInstanceCount()` | 中间层聚合现有模块（SpatialHashGrid + InstanceCullingBuffer）统一暴露；实例数据 bindless（对齐 GPUScene）（⚠️ `GetBucketMapSRV()`/`GetBucketOffsets()` 已废弃，2026-08-18 无桶流程 → `SetCullData`/`SetSegmentTables`） |

**基础设施强化（2026-08-10 检查定案）**：
- `SystemId = TaskId`（SystemTypes.h:26）已有唯一 ID——剔除层 system 可特化唯一 ID；
- `DependsOn` 依赖声明基于 **name 字符串**（SystemBuilder.h:36）——同名会混淆 → 剔除层 task 统一前缀
  （如 `"CullingSystem/OctreeQuery"`、`"CullingSystem/CSDispatch"`）+ 暴露 `GetTaskName()` 常量；
- 可选强化（排期评估）：`DependsOn(SystemId)` 重载绕开 name 字符串歧义。

**空间哈希更新（2026-08-10 定案）**：
- 双轨制已落地：静态 `Build(entries)`（场景加载，O(N) 零扩容）+ 动态 `AddEntity(entity, bounds, sceneId, ...)`（运行时 spawn，扩容兜底）；
- 实体 CRUD → `MarkDirty()` → PreCulling 阶段重建（与 RenderSlotCache::MarkDirty 同帧序）；
- `RemoveEntity(entity)` / `Clear()` / `SetEntityCullData(...)`（Build 后补录剔除元数据）齐备；
- 多 World/场景 ID：`AddEntity`/`Build` 均带 sceneId，剔除层按 `Cull(activeSceneId)` 过滤。

---

## 七、大型引擎对比验证（2026-08-10，官方文档）

| 结论 | 官方佐证 | 判定 |
|:--|:--|:--|
| 剔除系统=生产者/管道，渲染管线=消费者 | UE Nanite "只处理可见细节 + 材质段动态分配"；Unity BRG "draw commands 分离 + filter settings 控制渲染" | ✅ |
| 动态实体可 GPU 剔除化（<50 时 CPU 权衡） | BRG 批次每帧更新实例数据（instanceDataOffset） | ✅ |
| 分层剔除（粗筛→精筛→遮挡） | UE Visibility/Occlusion Culling：View Frustum + Occlusion Queries、Actor Bounds 球/盒两级 | ✅ |
| 材质槽=渲染管线（实例与材质解耦） | Nanite 材质段 + BRG 实例/材质解耦 | ✅ |
| LOD 选择在可见性/构建阶段（CPU） | UE 传统 LOD（InitViews CPU）、Unity LODGroup（Culling CPU） | ✅ |

---

## 八、已知缺陷与后续（记录）

1. **剔除结果消费**（补充 B）：ExecuteIndirect 链路已存在（OpaqueRenderer:232），readback 统计节流运行——需持续验证可见数正确性；
2. **allInstances 波动**：FrameSync 扁平化实例数随相机/粗筛波动（279~358），桶偏移表 total 可能 > 实例数（多桶展开）——语义核查待办（⚠️ 桶偏移表已随 2026-08-18 无桶流程废弃，本条桶语义不再适用，保留为历史）；
3. **动态实体 GPU 化**（§五）：数量增长时并入 L2（CullingDataStore 预留）；
4. **Nanite 模式**（§四记录）：虚拟化几何 + cluster 剔除为未来演进方向，当前不实施；
5. **三层抽象拆分**（§六）：目录骨架暂不建立，待代码拆分排期。

---

## 九、参考

- `../rendering/GPU-Drive.md`：L1/L2/L3 分层蓝图（块配置化 blockConfig ✅ 已落地）
- `OctreeCullingAndRaycaster.md`：空间哈希 + CulledSet + 双轨制（§7.5）
- `../rendering/RendererDataDriven.md`：Builder 桶模式 + FrameSync 统一上传
- `../rendering/LOD.md`：LOD 系统（离散句柄模型）
- `../core/EventSystemAndDataLayer.md`：SharedDataStore 中间层先例（三层抽象设计参照）
- UE Nanite / Unity BRG / UE Visibility and Occlusion Culling 官方文档（2026-08-10 检索）
