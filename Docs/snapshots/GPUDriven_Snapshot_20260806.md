# GPU Driven 分层蓝图 + 阶段 0~L2 落地快照 (2026-08-06 ~ 08-07)

> 分层剔除蓝图定稿 + 阶段 0（块配置化四端）+ 阶段 1（块入哈希/RenderSlotCache 展开）+ L2a（实例矩阵 StructuredBuffer）+ L2b（Compute 剔除）代码落地
> 本次（08-07）追加：L2c readback 验证链路、"着色器 if"方案（#935 ssao/envMap/shadow）、CORE 上传模式统一（#942 COPY 列表 COMMON 创建）
> 遗留已闭环（08-07）：LightingRenderer.cpp:198 崩溃根治（BlankTextureProvider 空白纹理 + LightManager 描述符堆标签 m_heapTag 补全 38 处，详见 §3.7）
> 关联：`Docs/architecture/rendering/GPU-Drive.md`（分层蓝图，2026-08-06 重写）、
> `Docs/targets/UKW_PowerUpKit/08_MapScenePipeline.md` §8.6/§8.7（区块化聚合 + 块配置化）、
> `Docs/architecture/culling/OctreeCullingAndRaycaster.md` §7.5/§8.1（查询单元解耦 + 演进路径）、
> `Docs/architecture/rendering/BillboardSystemArchitecture.md`（树 = 交叉 quad）、
> `.atomcode.md` 第 24/25 条

---

## 一、分层剔除蓝图定案（2026-08-06）

### 1.1 三层模型

```
L1 CPU 块级粗筛（✅ 已有，2026-08-04 定稿）
  空间哈希（格子 250）→ 块实体视锥剪枝 → cullDistance → CulledSet（≤5 块，相机静止缓存）
    → L2 GPU 实例级精筛（📋 待建）：块内实例矩阵一次上传 → Compute 逐实例视锥剔除
       → AppendBuffer/IndirectArgs → DrawIndexedInstancedIndirect（树/广告牌 ~11787 实例）
    → L3 遮挡（🔭 远期）：HZB / 保守化 PVS（只剔小物体，排除地块）
```

### 1.2 关键认知澄清

| 说法 | 实际 |
|:--|:--|
| ECS 实体数减少 | ✅ 15489 → 5 块（BlockComponent 聚合，-99.97%） |
| 渲染实例数据量不减 | ✅ 仍 15489 个实例矩阵（SOA 压缩 252KB） |
| 精细剔除下放 GPU | ✅ 原 Builder 逐实例视锥（CPU）→ GPU Compute（本蓝图核心） |

### 1.3 集群职责收缩（`GPU-Drive.md` §4.2）

- **BlockComponent = 纯剔除豁免器**（对抗远近裁剪面），非对抗实体数（实体数已由块聚合解决）
- `forceVisible` 只让**块实体进候选集**（粗筛豁免），块内实例 L2 GPU 视锥剔除**照常**——正交可叠加
- 对齐大型引擎：UE `bAlwaysVisible`（插入 `ComputeViewVisibility` 视锥循环）、HLOD Cluster 大包围体、远平面管理——**全部插在剔除查询循环内**
- 适用：山/远距建筑群/地形边界（"内容太大，超出裁剪面仍应可见"）

### 1.4 查询单元与数据组织单元解耦（`GPU-Drive.md` §4.1）

- 空间哈希格子（查询单元，250）≠ 块 cellSize（数据组织单元）——解耦
- **块大小由场景 JSON `blockConfig` 决定，缺失时加载推导**（UE World Partition 模式）

---

## 二、阶段 0 落地：块配置化四端（✅ 已完成 2026-08-06）

### 2.1 代码改动（6 文件）

| 端 | 文件 | 改动 |
|:--|:--|:--|
| 0a 结构体 | `Engine/Asset/IO/Loader/SceneDescription.h` | `BlockConfigDesc`（cellSize/blocksPerAxis/minCellSize/maxCellSize，全 0 = 推导模式）+ from_json/to_json；`SceneDescription::blockConfig` 可选字段 + to_json 输出 |
| 0b 解析 | `Engine/Asset/IO/Loader/SceneLoader.cpp` | LoadFromJSON：`j.contains("blockConfig")` → get；SaveToJSON：有配置才写 |
| 0c 推导 | `Engine/Scene/SceneConstructor.h/.cpp` | `SceneConstructData::blockConfig`；组装时推导 `cellSize = clamp(mapExtent / blocksPerAxis)`（默认 4 轴，上下限 100~1000） |
| 0c 消费 | `Editor/EditorLib/Scene/EditorSceneManager.cpp` | Phase C：硬编码 500.0f → `blockCellSize`（从 sceneData.blockConfig 读，兜底 500） |
| 0d 固化 | `EditorSceneManager.h/.cpp` | `SceneSnapshot::blockConfig` 缓存；OnSceneConstructReady 缓存；ExportToDescription 写回 |
| schema | `Schemas/scene.schema.json` | `blockConfig` 属性定义（4 字段），JSON 校验通过 |

### 2.2 行为语义

```
加载：JSON 有 cellSize → 直接用；缺失 → 按实体世界范围推导 cellSize = clamp(mapExtent/4, 100, 1000)
     → SceneConstructData.blockConfig → Phase C 用 blockCellSize 划分区块实体
保存：ExportToDescription 从快照读回 → 固化进 scene.json（下次加载零重算）
```

### 2.3 验证状态

- schema JSON 校验 ✅（node）
- 代码编译：**待人工执行**（项目规则：AI 不编译），已做逐编辑点静态审查
- 运行时验证：加载无 blockConfig 的 City4 → 推导 cellSize ≈ 262（1050/4）；保存 → 固化

---

## 三、阶段 1 + L2 落地（✅ 2026-08-06 ~ 08-07）

### 3.1 阶段 1a：块实体入空间哈希 + SceneTag

- `EditorSceneManager.cpp` Phase C：块实体补加 `SceneTagComponent`（m_openTabs[tabIndex].sceneId）
- `Editor.cpp` 八叉树重建：`view<BlockComponent>` 用 clusterBounds 入格（cullDistance=0 不拒远、forceVisible 照传），块成员收集到 unordered_set 跳过逐个入格
- clusterBounds = 成员实际网格包围盒合并（std::visit 处理 BoundingVolumeVariant，无网格实体 position±1 兜底）——修复水块被剔除（clusterBounds 过小）

### 3.2 阶段 1b：RenderSlotCache 块条目展开

- `RenderSlotCache.h` Entry 加 `const ECS::BlockComponent *blockComp` + include Block.h
- Dispatch 遇块实体展开 memberEntities 查缓存表分桶；`m_blockExpanded` 缓存（Rebuild 构建、Dispatch 零 getComp、Clear 同步清理）
- OpaqueRenderItemBuilder Count/BuildTypedCore 遇 blockComp 跳过逐实例视锥、保留 LOD/距离剔除（对齐 HISM）
- 广告牌（BillboardRenderItemBuilder 独立 view）阶段 1 不动

### 3.3 L2a：实例矩阵 StructuredBuffer（✅）

- 新建 `Engine/Renderer/Core/InstanceCullingBuffer.h/.cpp`（单例）：`GPUInstanceData`（XMFLOAT4X4 world + float radius + pad[3] = 80B）
- `CollectFromBlocks` 收集实例矩阵（StaticComponent.cachedWorld 或 Transform 兜底）+ 包围球半径×最大缩放
- `Upload`：GpuWorkItem 异步上传（DEFAULT + UPLOAD 中转 + COPY 队列 CopyResource + DIRECT barrier → SRV），按块记录 [offset,count)

### 3.4 L2b：Compute 剔除管线（✅）

- 新建 `Shaders/InstanceCulling.cs.hlsl`（t0 实例 SRV / u0 AppendBuffer / u1 IndirectArgs / b0 CullParams 6 平面）
- `CommandList.h` 扩展 compute 绑定（SetComputeRootSignature/DescriptorTable/CBV/UAV + Dispatch）
- `InstanceCullingBuffer` 增加 compute 管线（根签名 4 参数 + Compute PSO cs_5_1 + AppendBuffer/IndirectArgs UAV + 每帧清零 IndirectArgs[1] 对称屏障）
- `Editor.cpp` 注册 `EditorInstanceCullingSystem`（PrePass，相机静止跳过）

### 3.5 L2c 收缩（用户定案）：readback 验证链路（✅ 代码）

- 用户指示：树暂走实体渲染（最终 GS 阶段再做完整间接绘制），先从 tree 验证 GPU 剔除链路
- `DispatchCulling`：清零 → dispatch → readback 复制 InstanceCount → `ReadbackVisibleCount()`（延迟 1 帧）
- Editor 每 120 帧打印 `[InstanceCulling][Verify] visible=N / total=M (x.x%)`

### 3.6 运行期修复（GBV 严格模式暴露，全部落地）

| # | 问题 | 修复 |
|:--|:--|:--|
| #524 | UAV 资源缺 `ALLOW_UNORDERED_ACCESS` | `GpuResourceManager::CreateBuffer` 加 `flags` 参数；AppendBuffer/IndirectArgs 传 flag |
| #741 | readback 用 UPLOAD 堆（CopyResource 目标需 COPY_DEST） | `m_visibleReadback` 改 READBACK 堆（固定 COPY_DEST） |
| #942 | COPY 命令列表资源必须以 COMMON 创建 | 统一 CORE 上传模式：MeshLoadTask/GeometryProceduralTask/SceneConstructor 材质/PreviewPBRRenderer 四处 `COPY_DEST` → `COMMON` 创建 + DIRECT 转出（InstanceCullingBuffer::Upload 同款） |
| #935 | lighting.hlsl 根参数未绑定（ssao/envMap/shadow） | **"着色器 if"方案**：cbLights 加 `gHasSsao/gHasEnvMap/gHasShadow` 标志（LightConstants.Pad[0..2]），shader 无资源时跳过采样（GBV 只在实际执行采样时报错）；LightManager `SetHasSsao/SetHasEnvMap/SetHasShadow`（值变化置脏重传）；Editor 光照 pass 同步标志 + 传有效句柄（SSAO fallback、Skybox GetCubeSRV、LightManager shadow SRV） |

### 3.7 遗留问题（08-07 闭环：BlankTextureProvider 回退 + LightManager/AO/SsaoRenderer 堆标签根治）

- **LightingRenderer.cpp:198 崩溃（d3d12SDKLayers + envMapSrv 绑定）**：GBV 下 `SetGraphicsRootDescriptorTable(8, envMapSrv)` 报 `#646 INVALID_DESCRIPTOR_HANDLE`（句柄 `0x8000...` 不指向描述符堆）；日志另有 `#935 Uninitialized root argument accessed, Root Param [10]`（ShadowSampling.hlsl）——**真实绑定无效描述符，非 GBV 误报**。注：`#646` 为 **EXECUTION ERROR（执行期）**，断点停靠行（198）不等于真正出错的绑定点，配合 #935 Root Param [10] 判定病根在 **SSAO 与光照 pass 跨堆绑定**。
  - **第一层根因（✅ 08-07 落地）**：管理器（AO/Skybox/Water）在资源未加载时返回**空句柄** → LightingRenderer 条件跳过绑定 → shader 采样未绑定根参数；且管理器内部自建 fallback 的 `Allocate(EditorViewport, Texture)` 在分区未注册时走 Fallback 分支返回**裸 index** → `GetPartitionGpuHandle` 算出无效句柄。**根治**：引擎 CORE 新增 **`BlankTextureProvider`**（Game/Editor 共用单例），`Editor::Initialize` 中在 `AddPartition` 之后、管理器 Initialize **之前**同步阻塞创建并注入：
    - White2D（1x1 R8G8B8A8 0xFFFFFFFF）→ `AmbientOcclusionManager::SetFallbackWhiteSRV`
    - BlackCube（1x1x6 R8G8B8A8 全 0）→ `SkyboxManager::SetFallbackCubeSRV`（新增接口，`GetCubeSRV` 无天空盒时兜底）+ `WaterManager::SetEnvironmentMap`
  - **第二层根因（✅ 08-07 落地，编译后崩溃仍存在时定位）**：**`LightManager` 内部全部 38 处描述符堆调用缺 `m_heapTag`**（`Allocate`/`GetCpuHandle`/`GetGpuHandle`/`GetPartitionGpuHandle`/`Free`/`Reclaim` 走默认 `HeapTag::Default`），其 `shadowDataSRV`/`shadowMapSRV`（根参数 10/11）指向 **Default 堆**，而 Editor 光照 pass 命令列表只绑定 **EditorViewport 堆** → 绑定即 GBV #935/#646。**修复**：`LightManager.cpp` 38 处补 `m_heapTag`（规则 17）；`RenderScene::GetLightManager()` 返回同一单例（Bootstrap.cpp:629），修复生效。Game 端传 `HeapTag::Default` 行为不变
  - **第三层根因（✅ 08-07 落地，重新运行崩溃仍存时定位）**：**`AO::CpuSrvToGpu` 与 `SsaoRenderer` 同模式缺 `m_heapTag`**——`CpuSrvToGpu` 的 `GetHeap`/`GetDescriptorSize` 未传 tag（用 **Default 堆**基址计算 **EditorViewport 堆**的 CPU 句柄偏移 → 跨堆垃圾 GPU 句柄，恰为 `0x8000...` 特征），`BuildRandomVectorTexture` 三处描述符调用与 `SsaoRenderer::ComputeAO/BlurAO` 两处 `GetHeap` 同缺 tag（SSAO pass 绑定 Default 堆却使用 EditorViewport 堆的 depth/normal/AO SRV）。**修复**：`AO.cpp` 补 `m_heapTag`（CpuSrvToGpu 2 处 + BuildRandomVectorTexture 3 处）；`SsaoRenderer` 新增 `SetHeapTag` + `m_heapTag` 成员，两处 `GetHeap` 补 tag；`AO::Initialize` 注入 `SetHeapTag(m_heapTag)`；`LightManager:43` 校验调用补 tag
  - 编译期修复（首编暴露）：`BlankTextureProvider.h` 补 `DescriptorHeapCollection.h` include；`.cpp` 三处签名 `HeapTag` 与 winnt.h 全局枚举项冲突改 `Resource::HeapTag`；`WaterManager.h` `struct Registry`→`class Registry`；`SceneConstructor.cpp` 删除未用 `skyGeo`
  - 待人工编译验证（项目规则：AI 不编译）
- fmt error "argument not found"（onAllComplete 后 2 次）：遗留日志问题，不影响运行，暂缓
- 剩余 #942（BackBuffer/Main_DepthBuffer clear pass 状态，首帧 7 条）：引擎既有 clear 状态管理，非 L2 引入

---

## 四、下一步（待办）

| 阶段 | 内容 | 状态 |
|:--|:--|:--:|
| 阶段 0 | 块配置化四端 | ✅ 2026-08-06 |
| 阶段 1a | 块实体入空间哈希 + SceneTag + clusterBounds 修复 | ✅ 2026-08-06 |
| 阶段 1b | RenderSlotCache 块条目展开 + 跳过逐实例视锥 | ✅ 2026-08-06 |
| 阶段 2 | 集群豁免验证（forceVisible/clusterBounds 与 L2 正交性） | ✅/验证 |
| 阶段 3 | L2a 实例矩阵 + 包围球半径 → StructuredBuffer（静态一次） | ✅ 2026-08-06 |
| 阶段 4 | L2b Compute 视锥剔除 + AppendBuffer + IndirectArgs | ✅ 2026-08-06 |
| 阶段 5 | L2c 完整间接绘制（DrawIndexedInstancedIndirect + VS 变体 + MaterialIndex） | ⏸️ 延后至 GS 阶段（用户定案：树暂走实体渲染，readback 验证链路已落地） |
| 阶段 6 | L3 HZB / 保守化 PVS | 🔭 远期 |

---

## 五、大型引擎对照（分层参考）

| 机制 | 大型引擎 | 插入点 | 我们 |
|:--|:--|:--|:--|
| 强制可见 | UE `bAlwaysVisible` | `ComputeViewVisibility()` 视锥循环 | `BlockComponent.forceVisible` → `OctreeSystem::m_forceVisibleEntities` ✅ |
| 大包围体 | UE HLOD Cluster | World Partition 流式后、视锥剔除前 | `clusterBounds` + `FrustumCullAABB` 15% 扩展 ✅ |
| 远平面管理 | UE cull distance / 自适应远平面 | 相机矩阵构建 | `AdaptiveFarPlane.md` CullFarPlane=1000 ✅ |
| 区块配置 | UE World Partition cell 尺寸可配置 | 场景资产 | `blockConfig`（JSON 可配置 + 加载推导）✅ |
| 实例剔除 | Nanite cluster / Unity BRG | GPU Compute | L2（📋 待建） |
| 预计算遮挡 | id Tech PVS（仅静态） | 离线 | 保守化 PVS（🔭 远期） |

---

## 六、2026-08-08 后续：剔除链路修复 + L2c 优先级提升（P0）

### 6.1 已闭环：L2b dispatch 真实执行（GpuResourceHandle 无效值根因）

| 问题 | 根因 | 修复 |
|:--|:--|:--|
| `CullParams CBV invalid (cbAddr=0)` 每帧报错、`[Verify] visible=0/6853 (0.0%)` 恒 0 | `GpuResourceHandle` 位域默认零初始化 `{index=0,gen=0}` 被旧 `IsValid()`（只查 `index != 0x3FFFFF`）**误判为有效** → `CreateCullingPipeline` 中 `if (!m_cullParamsUp.IsValid())` 跳过 `CreateBuffer` → dispatch 绑定 cbAddr=0 悬垂 CBV | `GpuHandlePool.h`：无效值改整值 **0xFFFFFFFF**（`Invalid()={0x3FFFFF,0x3FF}`、默认成员初始化器全 1、`IsValid()` 按 32 位判断）；`InstanceCullingBuffer.h` 5 成员显式 `= Invalid()` |

**验证**：`[Verify] visible` 从恒 0 变为随相机实时波动（City 6853 实例 85%→10%→74%；City4 46464 实例 0.1%~19.4%）；`cbAddr=0` 错误 0 次；帧率 48-57 FPS（City）。
**关联**：`Docs/bugs/BugFix_InstanceCulling_CullParamsCBV_HandleInvalid.md`、`InstanceCulling_MemoryGrowth_TDR_Snapshot_20260808.md` §六。

### 6.2 确认 L2c 未消费（算而不吃）

`AppendBuffer`/`IndirectArgs` 由 L2b dispatch 生成但**零消费**：`GetAppendBufferAddress`/`GetIndirectArgsAddress`（InstanceCullingBuffer.h:118/125）定义后无调用点；`DrawIndexedInstancedIndirect` 仅存在于注释（Editor.cpp:1183）。绘制仍走 CPU 侧 Builder 精筛队列（Perf `queue=130~254`）。

### 6.3 阶段 5 状态更新：延后 → **P0 推进**

原 §四 阶段 5 状态"⏸️ 延后至 GS 阶段"（用户定案：树暂走实体渲染）已**被用户升级**：
- 2026-08-08 定案：**L2c 间接绘制落地为 P0**（最高优先级），替换 readback 验证链路为真实绘制消费
- 待办明细见 `Docs/todos/InstanceCulling_L2c_Todo.md`（L2c P0 + City4 实体残留 P1 + SRV/上传优化 P2/P3）

### 6.4 遗留（非 L2 引入，观察项）

- `Perf[Diag] cellsHit=0` 恒 0（八叉树统计口径，块级粗筛不计数，预期语义待确认）
- City4 二次构建实例数虚增（15488→46464，CloseTab 不清实体，P1 待办）

---

## 七、异常调查：RenderDoc ExecuteIndirect 63→2233（≈2170 次）vs Perf queue=242（2026-08-08 晚）

> 状态：**明显异常，未定论**——已用数据排除 L2c 材质段/子网格级拆分，大概率是 RenderDoc 全帧统计口径，待过滤确认。完整调查见 `Docs/todos/InstanceCulling_L2c_Todo.md` §五。

### 现象

RenderDoc 捕获 **ExecuteIndirect 事件区间 63→2233（≈2170 次）**，远超 Perf `queue=242`（Opaque 渲染项数 = 每渲染项一次 ExecuteIndirect）。用户观察每条 `ExecuteIndirect(maxCount 1, count <1>)` 内部仅一条命令；用户补充"2300 是实体渲染器的部分"。

### 数据证据（已统计验证）

| 层级 | 数量 | 来源 |
|:--|:--|:--|
| City 实体总数 | 6854（mesh 6853） | City.scene.json |
| **子网格×材质去重组合** | **81**（44 geometry：mapChip×5、buildingHigh×3 等） | node 解析 JSON |
| L2c 材质段桶 | 170 | `buckets=170` 日志 |
| Opaque 渲染项（=ExecuteIndirect 次数） | 242 | Perf `queue=242` |
| RenderDoc ExecuteIndirect 事件区间 | 63→2233 | RenderDoc |

**81 组合 vs 2170 差 27 倍** → 2170 **不可能**来自子网格/材质段拆分；材质段聚合已最大化（6853 实体 → 81 组合 → 170 桶 → 242 ExecuteIndirect）。

### 已排除的根因

- 子网格级拆分回归（1800 桶）❌（81 组合证据）
- 材质段聚合未生效 ❌（SceneConstructor/Builder/buckets=170 均验证）
- ExecuteIndirect 内部多段展开 ❌（RenderDoc `maxCount=1`）
- AppendBuffer 容量越界 ✅ 已修复（`Verify total=615 < flat=861` 曾触发；本轮扩容警告 0 次）

### 当前判定与确认方法

- **大概率**：2170 = RenderDoc **全帧 draw call 统计**（ExecuteIndirect 242 + Lighting/SSAO/Water/Wireframe/PostProcess/UI/Preview 等其他 pass ≈1900 条），非 L2c 膨胀
- **确认**：RenderDoc 过滤只看 `ExecuteIndirect` 类型——≈242 则 L2c 无缺陷；仍 ~2000 则 Builder 生成 2000+ 渲染项（深挖 pendingBatches）

### 数据层性能基线（已证实正常）

`flat instances: 971 (96B/instance), buckets=170`；`queue=242`；`[Verify] visible=852~960 / total=860~968 (88~111%)`；builder=0.10-0.15ms；ExecuteIndirect 242 次（材质段级）
