# GPU 驱动阴影剔除方案（GPU-Driven Shadow Culling）

> **状态（2026-08-27）**：⏸ **尚未回归**——2026-08-18 无桶流程重构后，阴影 dispatch 循环与阴影渲染
> 在 `EditorRenderCullingSystem.cpp` 整块注释停用；CS 阴影分支（`gCullTask==1`）代码保留，恢复时需按
> 无桶流程（段表 + 一次 ExecuteIndirect）重新接线。文中"材质桶段"等桶相关内容属旧架构。
> **阶段定位**：S7 阴影剔除——每光源独立 dispatch，输入 = 大一统候选集（主视口 ∪ 光源候选集）。
> 阶段表见本目录 `README.md`
>
> 创建：2026-08-13。阴影在 GPU 剔除模式下的应用方案——三阶段剔除架构
> （主视口 / 阴影 / 光源）+ 大一统候选集 + 每光源独立绘制输出。
> 关联：`GPU-Drive.md`（GPU Driven 剔除分层蓝图）、`TwoPassHZB.md`（HZB 遮挡剔除）、
> `InstanceCullingSystem.md`（剔除管道）、`Docs/architecture/rendering/shadow.md`（阴影演进方向）、
> `Docs/architecture/core/SnapshotSystem.md`（GTA 查询计数器模式）。

---

## 一、背景与目标

### 1.1 现状差距

| 端 | 阴影状态（2026-08-13 实证） |
|:--|:--|
| **Game 端** | 三光源阴影已正确实现（方向光单张正交 2048 / 点光 GS 6 面 1024 / 聚光单面 1024），但**渲染 = CPU 遍历 `m_opaqueQueue` 逐个 DrawInstanced** 画深度——无剔除 |
| **Editor 端** | **无阴影渲染系统**（`Editor.cpp:2337` 注释实证："Editor 未创建阴影贴图（无阴影渲染系统）→ gHasShadow=0，shader 跳过 SampleShadow"） |
| **GPU 剔除** | `CullData.shadowViewMask` 已预留（偏移 24B，低 16 位 CSM + 高 16 位点光/聚光），注释明确"多视角各一份 CullResult，复用同一 CullData，无数据依赖，多 dispatch 并行"——**当前未使用** |

### 1.2 目标

在 GPU 剔除模式下实现阴影：**让 GPU 为每个光源视角独立计算可见性**（每光源一个"迷你相机"），
输出每光源可见实例列表驱动阴影贴图渲染——替代 CPU 全队列遍历。

---

## 二、三阶段剔除架构（定案）

> **2026-08-27 注**：下表"输出数据"列（`shadowAppend_光源` / `gIndirectArgs` / `shadowIndirectArgs_A/_B`）
> 均属 **2026-08-18 之前桶流程**的数据结构，已随无桶流程（一次 `ExecuteIndirect` + 段表）废弃。
> 阴影剔除语义本身（每光源独立 dispatch + 大一统候选集）仍是回归方向，恢复时数据结构按无桶流程重设计。

| 剔除类型 | 输入数据 | 输出数据 | 用途 |
|:--|:--|:--|:--|
| **主视口实例剔除** | 场景中所有实例（建筑、地形、角色等） | 主视口可见实例列表（gAppend/gIndirectArgs） | 决定"画什么" |
| **阴影剔除** | **全量实例**（主视口候选 ∪ 光源候选，大一统候选集） | 每光源/级联可见实例列表（shadowAppend_光源） | 决定"为哪些物体生成阴影贴图" |
| **光源剔除** | 场景中所有光源（方向光、点光、聚光） | 对当前视口有贡献的光源列表 | 决定"哪些光源参与光照计算/阴影渲染" |

### 2.1 关键正确性（2026-08-13 讨论定案）

**阴影剔除输入 = 全量实例（非主视口可见列表）**：

```
场景：物体A在相机背后，但阳光可照到并投影到可见地面
  主视口剔除：A 在主视锥外 → 不在主视口可见列表
  若阴影剔除输入 = 主视口可见列表：A 永不进入阴影贴图 → 地面阴影缺失（pop-in）
```

**为什么"针对剔除结果的剔除"在此不成立**：阴影可见集 ≠ 主视口可见子集——
它是 **"主视口可见 ∪ 能从光源投影到可见区域的物体"**（含相机背后的 caster）。
大型引擎（UE `FShadowDepthPass` / Unity HDRP `RenderShadowMaps`）均用**独立阴影视锥剔除**，
不依赖主视口可见性。

**GPU 成本可接受**：阴影剔除**无 HZB**（只做光源视锥球测试）→ 单次 dispatch 极便宜
（~0.02-0.05ms），多次分发可接受（多光源 = 多次 dispatch 是行业标准，HLSL 无法动态
切换资源绑定，每光源独立资源组）。

### 2.2 光源剔除（L0 前置）

光源作为实例处理（复用球测试逻辑）：光源包围球（位置 + Range/衰减半径）vs 主视锥 →
输出"贡献光源列表"→ 驱动：
1. **光照计算**：只算贡献光源
2. **阴影渲染**：只对"贡献且 CastShadow"的光源 dispatch 阴影剔除

与现有 `CullingData`（worldPos + boundingRadius）天然同构——复用同一剔除框架
（光源实例池 + 光源 CullData 段）。

---

## 三、大一统候选集 + GTA 合并（输入合并）

### 3.1 关键区分：候选集 ≠ 绘制列表

```
CPU 粗筛（L1）→ 候选集（块列表）→ GPU 剔除（L2）→ 绘制列表（gAppend/gIndirectArgs）→ ExecuteIndirect 绘制
```

**绘制的唯一驱动源 = gIndirectArgs（InstanceCount）**——候选集只是剔除的输入，
候选集变大**不会多画**（只增加 GPU 剔除处理量，不增加绘制量）。

### 3.2 大一统候选集（定案）

```
L1 CPU 粗筛（主视口 + 光源，GTA 合并去重）
  → 大一统候选集（一份：主视口候选 ∪ 光源候选块）
      ↑ 实体只被处理一次（GTA：g_queryStamp 递增 + lastQueryStamp 比对，O(1) 摊销）

L2 GPU 剔除（共享同一候选集，各自输出独立绘制列表）：
  主视口 dispatch → 视锥 + HZB → gAppend（主绘制列表）
  光源 A dispatch → 光源视锥（无 HZB）→ shadowAppend_A
  光源 B dispatch → 光源视锥 → shadowAppend_B
  ...

绘制：
  主渲染：ExecuteIndirect(gIndirectArgs 主)
  阴影渲染：ExecuteIndirect(shadowIndirectArgs_A) / _B / ...
```

**要点**：
- **输入一份（大一统候选集）**——不重复，GTA 合并；
- **输出多份（主 gAppend + 每光源 shadowAppend）**——必要（各自绘制列表），非重复；
- **绘制只消费各自输出**——候选集再大也不会多画；
- **每光源独立实例集则重复且违背 GTA 合并意图**——不采用。

### 3.3 GTA 查询器作用边界

| 成本项 | GTA 是否消除 | 说明 |
|:--|:--|:--|
| **数据准备**（遍历实体、更新包围盒、访问空间哈希索引） | ✅ 去重 | 同帧多剔除器查询实体只处理一次（O(1) 摊销，替代每帧 unordered_set） |
| **光源×实体视锥判定** | ❌ 不消除 | 判定计算本身放 GPU（每光源 dispatch），CPU 不做 O(候选×光源) |

---

## 三.5 多视锥 CPU 粗筛（2026-08-14 定案：剔除层视锥缓存 + 遍历剔除）

> 背景：实现排查发现 **L1 CPU 粗筛只查询主相机视锥**（`OctreeCullingSystem` 仅
> `QueryFrustum(cullingSys.GetFrustum(), m_octreeCoarse, ...)`），光源视锥从未参与 CPU 粗筛 →
> 阴影 dispatch 输入（`allInstances` = Builder 桶 = 主视口 CulledSet 展开）**不含光源视角内容**
> （相机背后投影物缺失）→ 阴影贴图空/正确率低。本节修复：剔除层提供**视锥缓存 + 遍历剔除**
> 能力，粗筛输出 = 主视口 ∪ 光源视锥的大一统候选集（对齐 §3.2 设计）。

### 3.5.1 接口（SpatialHashGrid + CullingLayer 门面转发）

| 接口 | 语义 |
|:--|:--|
| `SetFrustum(uint32_t key, const Frustum &frustum)` | 设置/覆盖缓存视锥（key=0 主视锥，1..N 光源/级联） |
| `UpdateFrustum(uint32_t key, const Frustum &frustum)` | **单视锥更新**（视锥每帧可能变——光源方向/相机移动，只更新对应 key，不触碰其他） |
| `ClearFrustum(uint32_t key)` / `ClearAllFrustums()` | 移除视锥（光源移除/场景切换） |
| `QueryAllFrustums(CulledSet &outSet, const XMFLOAT3 &refPos)` | **遍历全部缓存视锥 → 并集候选集**（GTA 去重） |
| `QueryAllFrustumsChunk(..., chunkIndex, chunkCount)` | 并行块版（块内串行遍历所有视锥，块间并行） |

视锥缓存内部为 `std::unordered_map<uint32_t, Frustum>`（容量 ≥ `kMaxShadowLights+1`，超限忽略）。

### 3.5.2 GTA 共享 stamp（关键正确性）

```
QueryAllFrustums:
    ++m_frustumStamp（只递增一次！）
    for each cached frustum:
        遍历覆盖格子 → 实体球测试
        stamp 比对：已并入（stamp == m_frustumStamp）→ 跳过；否则并入 outSet
    forceVisible 实体并入（块级豁免，不重复）
```

**一次递增 + 全视锥共享** = 实体命中任一视锥即进并集，O(1) 去重——正是 §3.3 GTA
"同帧多剔除器查询实体只处理一次"的设计意图。复用现有 `m_queryStamps`，零新增状态。
（注：每视锥独立 `++m_frustumStamp` 会使第二视锥重新处理同一实体 → 重复入集，必须共享。）

### 3.5.3 refPos / cullDistance 语义

| 视锥 | refPos（cullDistance 拒远基准） | 说明 |
|:--|:--|:--|
| 主视锥（key=0） | 主相机位置 | 保持现状：离主相机超过 cullDistance 强制剔除 |
| 光源视锥（key=1..N） | **光源位置**（或跳过 cullDistance） | 阴影剔除不应被主相机距离限制——相机背后投影物（光源可见）必须进入候选集；定案：**光源视锥跳过 cullDistance 拒远**（全量实例输入语义，§2.1） |

### 3.5.4 调用点改造

| 位置 | 改造 |
|:--|:--|
| `OctreeCullingSystem`（PreCulling） | 相机移动时：`SetFrustum(0, 主视锥)` + `SetFrustum(1..N, 光源视锥)` → `QueryAllFrustums(m_octreeCoarse, camPos)` 替代单视锥查询 |
| 光源视锥数据源 | `LightManager::GetDirShadowLightViewProj()` 在 PreCulling 前已可用（`UpdateAndUpload` Immediate 回调 → `RebuildShadowConstants` → `ComputeDirShadowMatrix`）——与 GPU 侧 shadowFrustumsUp 同源 |
| 并行块（EditorCullChunk0-3） | `QueryAllFrustumsChunk`（块内遍历所有视锥） |
| 静止缓存判定（`m_cameraMoved`） | 主视锥 ViewProj 不变 + 光源视锥不变（比较 `GetDirShadowLightViewProj` 缓存）→ 复用 coarse |

### 3.5.5 预期效果

- **主相机候选集为空 → 光源视锥仍有内容**：遍历光源视锥，相机背后投影物进入候选集 → 阴影 dispatch 输入不再依赖主视口粗筛
- **阴影贴图正确率提升**：光源可见实例进入 allInstances → 阴影剔除 CS 输入完整
- **对齐 §3.2 设计**：实现"L1 CPU 粗筛（主视口 + 光源，GTA 合并去重）→ 大一统候选集"

---

## 四、数据流与资源布局

### 4.1 数据流（PrePass 阶段）

```
① CPU：光源剔除（L0，包围球 vs 主视锥）→ 贡献光源列表 + 阴影视锥收集（方向光 1 + 点光 6 面 + 聚光 1）
② CPU 粗筛（L1，GTA 合并）→ 大一统候选集（CullData 段，与主视口共享）
③ GPU 剔除（L2，多 dispatch）：
   主视口 dispatch → 视锥 + HZB → gAppend/gIndirectArgs（现状不变）
   每光源 dispatch → 光源视锥球测试 → shadowAppend_光源/shadowIndirectArgs_光源
④ 阴影渲染（Editor 新增 ShadowRenderSystem，复用 Game 端 ShadowRenderer）：
   按 shadowIndirectArgs_光源 ExecuteIndirect → 深度渲染阴影贴图
⑤ Opaque 主渲染（ExecuteIndirect 主）
```

### 4.2 shadowViewMask 澄清（2026-08-13 实证）

`CullData` 缓冲为 **UPLOAD 堆**（`CullingResourceManager.cpp:203`）——CS 只读 SRV，
**无法写回 shadowViewMask**（UPLOAD 堆不能建 UAV）。因此：
- `shadowViewMask` 字段保留为 CPU 上传的初始 0（语义占位）；
- **剔除结果写入独立 shadowMask UAV 缓冲**（DEFAULT + ALLOW_UNORDERED_ACCESS，uint32/实例）——
  CS 写位（每 thread 只写自己槽位，无原子竞争）、阴影渲染 SRV 读；
- 或直接采用**每光源独立 shadowAppend**（本方案推荐，见 §四.3）。

### 4.3 资源布局（每光源一组）

| 资源 | 类型 | 数量 | 说明 |
|:--|:--|:--|:--|
| `CullData`（候选集） | UPLOAD 只读 SRV | 1 | 大一统候选集，主视口与所有光源 dispatch 共享（零重复上传） |
| `gAppend`/`gIndirectArgs` | DEFAULT UAV/SRV | 1 | 主视口绘制列表（现状） |
| `shadowAppend_光源`/`shadowIndirectArgs_光源` | DEFAULT UAV/SRV | 每光源 1 组 | 阴影绘制列表（新，对齐 AppendBuffer 模式） |
| `shadowMask`（可选） | DEFAULT UAV/SRV | 1 | 若按位掩码而非独立 append（备选） |
| 阴影视锥缓冲 | UPLOAD/SRV | 1 | `StructuredBuffer<ShadowFrustum>`（每组 6 × float4 平面） |

**容量**：`CullData` 1MB ≈ 21845 条，实测剔除输入 924~1098，余量充足（2026-08-13 日志实证）。

### 4.4 生命周期（纳入 CullingResourceManager）

新增阴影资源（shadowAppend/shadowIndirectArgs/阴影视锥）纳入 `CullingResourceManager`
生命周期，对齐 AppendBuffer 模式（规则 11：只管生命周期，状态由使用方对称屏障）：

| 环节 | 处理 |
|:--|:--|
| 创建 | `CreateUAVs` 扩展（按光源数分配 shadowAppend/IndirectArgs + UAV 槽位） |
| 每帧更新 | 阴影视锥上传（Map 重写）+ CS 原子写 shadowAppend（对齐 gAppend 语义） |
| 扩容 | 光源数增长 → 重建 shadowAppend 组（fence 延迟释放旧资源，对齐 #921 修复模式） |
| 释放 | `ReleaseCullingResources` 扩展（shadowAppend/IndirectArgs + 槽位） |

---

## 五、与现有架构的衔接

| 端 | 改动 |
|:--|:--|
| `CullingResourceManager` | 新增 shadowAppend/shadowIndirectArgs（每光源一组）+ 阴影视锥缓冲 + UAV 槽位 + 生命周期 |
| `CullingRenderer` | `DispatchCulling` 扩展：阴影视锥参数 + shadowAppend u2 UAV 绑定（多光源多次调用） |
| `InstanceCulling.cs.hlsl` | 新增阴影视锥 StructuredBuffer 输入 + 光源视锥球测试输出（可复用现有视锥测试逻辑，无 HZB） |
| `Editor.cpp` | 新增 `EditorShadowRenderSystem`（复用 Game 端 ShadowRenderer）+ 光源剔除（L0）+ 阴影视锥收集 + 阴影贴图创建（复用 `LightManager::CreateShadowMapForDirectionalLight/PointLight/SpotLight`，Game.cpp:94-101 先例） |
| 阴影贴图 | Editor 复用 LightManager 创建三光源阴影贴图 → `SetHasShadow(true)` → shader 启用 SampleShadow |

---

## 六、实施步骤

- [x] **结构定案（2026-08-13）**：CullParams 288B（gViewPlanes[6] 保留主视锥 + gPlanes[6] 当前任务视锥 +
      gViewProj + gScreenSize + gHzbMipCount + gInstanceCount + gCullTask + gLightIndex）；ShadowFrustum 结构；
      三输出（gAppend/gShadowAppend[光源]/gLightAppend）；根签名 10 参数（t0-t3 + u0-u4）；DispatchCulling
      签名加 cullTask/lightIndex；
- [x] **HLSL 分流（2026-08-13）**：拆分为可复用工具函数（FrustumCullSphere 三任务共用 /
      HZBOcclusionTest + AppendToBucket 主视口专属）+ CSMain 按 gCullTask 分流（0 主视口 / 1 阴影 / 2 光源）；
- [x] **资源层（2026-08-13）**：CullingResourceManager 新增 shadowFrustumsUp（UPLOAD + SRV t3）、
      每光源 shadowAppend/shadowIndirectArgs（DEFAULT UAV，kMaxShadowLights=8）+ UAV 槽位、lightAppend（UAV u4）、
      生命周期（CreateUAVs 创建 + ReleaseCullingResources 释放）；
- [x] **dispatch 接线（2026-08-13）**：CullingRenderer::DispatchCulling 阴影/光源任务精简路径——
      gCullTask==1 绑 t3 shadowFrustums SRV + u2 shadowAppend UAV（当前光源），gCullTask==2 绑 u4 lightAppend；
      无 HZB/桶偏移/readback；计数头清零 + 规则 10 对称屏障；
- [ ] **1. Editor 接入阴影渲染（基础）**：复用 ShadowRenderer + LightManager 创建三光源阴影贴图，
      EditorShadowRenderSystem（CPU 遍历队列同 Game 端）——先让 Editor 有阴影，验证画面；
- [ ] **2. 大一统候选集确认**：验证候选集共享（主视口 + 光源 dispatch 复用同一 CullData 段）；
- [x] **3. Editor 阴影视锥收集（2026-08-13）**：LightManager 加 GetDirShadowLightViewProj /
      GetSpotShadowLightViewProj getter（m_shadowParams Type 过滤）→ Frustum::BuildFromMatrix 提取
      6 平面 → Editor 每帧 Map 上传 shadowFrustumsUp（固定地址；方向光 + 聚光，isValid 过滤）；
- [x] **4. Editor 每光源 dispatch（2026-08-13）**：贡献光源循环调用 DispatchCulling(cullTask=1, lightIndex=i)
      （方向光 + 聚光已接；点光 6 面视锥待补 GetPointShadowLightViewProj getter；光源剔除 L0 cullTask=2
      的 HLSL/资源侧已就绪——gLightAppend + u4 绑定，Editor 调用点待接）；
- [x] **5. 阴影渲染消费（2026-08-13 编辑器端已实施；Game 端暂缓）**：EditorShadowRenderSystem
      （RenderPhase::PrePass）复用实体 Builder 桶数据（m_opaqueQueue 每桶 RenderItem）+
      阴影剔除结果（shadowIndirectArgs_光源 桶段 InstanceCount）逐桶 ExecuteIndirect：
      ```
      EditorShadowRenderSystem：
        1. 首次创建方向光阴影贴图（CreateShadowMapForDirectionalLight 2048，Game.cpp:95 先例）
        2. BeginOffscreen（DSV/视口/裁剪——ShadowRenderer）
        3. 逐桶：ExecuteIndirect(shadowIndirectArgs_光源 桶段)——t12 InstanceData + t13 gShadowAppend
           （DirShadowVS_Indirect 间接索引，复用实体桶数据，与主视口 color.hlsl gAppend 同模式）
        4. EndOffscreen + 规则 26 命令池 Acquire→Release 配对
      ```
      配套改动：Shadow.hlsl 新增 gShadowAppend（t13,space1）+ DirShadowVS_Indirect（间接索引变体，
      Game 端 DirShadowVS 直接索引保留）；ShadowRenderer 根签名加 slot 4（t13）+ ExecuteIndirect 方法
      （D3D12_DRAW_INDEXED_ARGUMENTS 命令签名 CreateIndirectCommandSignature，Initialize 调用）；
      Editor 持有 m_shadowRenderer 实例（Initialize 初始化）；Game 端 ShadowRenderSystem 后续按同模式改造；
      **运行期修复**：①#743 命令签名根签名传 nullptr（DRAW_INDEXED 不改变根参数，对齐 OpaqueRenderer）
      ②#646 DSV handle 缺 HeapTag（规则 17：补 HeapTag::EditorViewport，对齐 LightManager 创建）
      ③#538 ShadowMap_Depth 缺状态屏障（规则 10：入口 COMMON→DEPTH_WRITE + 出口 DEPTH_WRITE→COMMON，
      DepthStencilPool 创建初始 COMMON）④槽位重叠（m_shadowIndirectArgsUavIndex{} 全 0 → 构造函数
      补 fill(UINT32_MAX)，与 m_shadowAppendUavIndex 同类——首次 ReleaseCullingResources 误 Free 槽位
      0-7 → allocator 重叠 → u0/u1 绑定 Culling_ShadowIndirectArgs）⑤**GBV #961 CullParams 缓冲越界
      （2026-08-13 终极根因）**：CullParams 结构 288B（gViewPlanes/gPlanes/viewProj/screenSize/mipCount/
      instanceCount/cullTask/lightIndex/pad）但 CreateCullingPipeline 创建缓冲仅 **256B** → 阴影 dispatch
      的 CS 访问偏移 279（cullTask/gLightIndex 区域）越界 → gCullTask 读取未定义值 → 阴影分支不执行
      → shadowIndirectArgs InstanceCount=0 → ExecuteIndirect 空调用 → 阴影图全白 + GBV 错误干扰
      RenderDoc/Nsight 抓帧（RenderDoc 显示 IndirectDrawIndexed(<N,0>) 实为伪影，根因是越界）；
      修复：两处缓冲 256B→**288B**（主视口 + 阴影，HLSL/C++/缓冲三方 288B 一致）；
- [x] **阴影剔除桶分段实施（2026-08-13）**：HLSL 新增 ShadowAppendToBucket（按 gBucketMap 桶归属递增
      gShadowIndirectArgs[segBase+1] InstanceCount + 按桶偏移表写 gShadowAppend 桶段，对齐主视口
      AppendToBucket 去 HZB/非零桶区/readback）；CullingRenderer 阴影分支双输出（u2 shadowAppend +
      u3 shadowIndirectArgs，每光源一组）——入口 INDIRECT_ARGUMENT→COPY_DEST，COPY bucketArgsUp 前部
      （静态字段+InstanceCount 归零）+ bucketOffsetsUp 尾部（桶偏移表）→ COPY_DEST→UAV → Dispatch →
      出口 UAV→INDIRECT_ARGUMENT（规则 10 对称）；CullingResourceManager shadowIndirectArgs 布局对齐
      主视口 gIndirectArgs（前部 5*kMaxSubMeshRanges*kMaxCullBuckets + 尾部桶偏移表 kMaxCullBuckets+1）
      + 每光源 UAV 槽位 + 释放；
- [x] **castsShadow 承担作用（2026-08-13 用户定案）**：地形/树等**不投射阴影**——CullData 复用
      pad0 槽位改 `castsShadow`（HLSL/C++ 同步，80B 布局不变）+ Editor FrameSync 填充
      （`cd.castsShadow = meshComp->castsShadow`）+ 阴影剔除 CS 过滤（`inst.castsShadow==0` 不写入
      shadowAppend → 不渲染进阴影贴图，避免自身深度临界伪影）+ shadow_test 地面 castsShadow=false
      （16 立方体 true + 地面 false）；语义：投射=进阴影贴图（CS 过滤），接收=光照阶段采样（ReceiveShadow）；
- [ ] **采样无阴影待修（2026-08-13 调查定位，①已修复 ②待验证）**：
      ① ~~light.Direction 硬编码~~（**已修复 2026-08-13 P0 + 用户定案修订**）：快照重建路径
      EditorSceneManager.cpp:1302 原硬编码 `light.Direction={0,-1,0}`（垂直向下 → LookAtLH up 平行
      forward 退化 → 阴影贴图异常）——修复方案历经两版：先加 LightDesc.direction 字段（四端），
      **用户定案回退**（参考 UE/Unity：光源旋转与投射方向不应是两码事）→ 最终改为**光源方向 =
      Transform.rotation 单一方向源**：快照重建从场景光源实体（snap.entityDescs）的 transform.rotation
      计算（XMVector3Rotate(forward=(0,0,1), quat)，对齐 Editor.cpp:621-627 主路径）；LightDesc.direction
      字段已回退移除；**up 退化防御**（ComputeDirShadowMatrix：|dot(lightDir,up)|>0.999 换 up=(1,0,0)）保留
      ② **描述符绑定错位**：RenderDoc 实证 t15 gReflectionCubemapArray 绑到 Hzb_MipChain（2D 纹理）——
      t14 gShadowMaps 无界数组基址（GetShadowMapSRV=Shadow 分区槽位 0）与方向光 SRV 实际槽位
      （m_dirShadow.srvSlot=Allocate(Shadow)，ShadowMapIndex=srvSlot@488）对应关系待验证；用户确认
      space1,14 gShadowMaps[1022]=ShadowMap_Depth（2048 2048 R32_FLOAT）**绑定正确**；
      ③ gWorldPosRT 已确认正确写入（color.hlsl SV_Target3=WorldPos）——G-buffer 世界坐标通道正常，
      延迟光照 worldPos 不依赖主相机深度（gWorldPosRT t23 独立通道，非深度重建）；光照 PASS 无需绑定 DSV
      ④ **shadow_test 场景 rotation 180° 错误（2026-08-13 修复）**：场景文件实体 rotation `[0,1,0,0]`
      （绕 Y 轴 180°，非单位四元数——复制模板遗留）→ 场景相对天空盒旋转 180° → 完全背光 → 无阴影；
      修复：17 处实体 rotation 改单位四元数 `[0,0,0,1]`（Sun 保留 45° `[0.38268,0,0,0.92388]` 合法值）；
      约定：rotation 四元数顺序 **[x,y,z,w]**（SceneConstructor.cpp:700 + TransformDesc 注释），
      无旋转实体必须用 `[0,0,0,1]`（w=1），禁用 `[0,1,0,0]` 类 180° 值
- [x] **阴影验证成功 + 批量阴影安排（2026-08-13）**：City 场景**已观察到阴影**（全链路打通——
      阴影剔除桶分段 → ExecuteIndirect → DirShadowVS_Indirect → 光照深度重建采样）：
      ①测试期：添加 20 个 ShadowCube（原点 ±16 网格，castsShadow+receivesShadow）+ ShadowGround
      （procedural://grid/200/200/50/50 程序化平面，原点）验证阴影投影；
      ②清理：node 脚本（scripts/fix_city_shadows.js，环境无 python 用 node）删除 21 个测试实体
      + ShadowCube 依赖（6875→6854 实体）；
      ③批量安排：为 413 个 building* 实体添加 castsShadow:true + receivesShadow:true
      （408 新增 + 5 先前已加；写入前备份 City.scene.json.bak）；
      ④脚本模式：大文件批量处理用 node（JSON.parse/stringify 4 空格缩进，结构安全）；
      后续可按需扩展（tree/mapChip 等命名模式批量）
- [x] **bucketOffsetsUp 三帧 RingBuffer 化（2026-08-13 用户定案）**：每帧变化数据（桶偏移表前缀和）改
      **三帧 ring 缓冲**（容量 ×3，帧段 stride = kMaxCullBuckets+1）而非 GPU 资源管理器单缓冲——
      消除 GPU 异步 COPY 与 CPU 每帧 Map 覆盖的跨帧竞争（阴影闪动嫌疑）：
      ```
      SetFlatInstances（FrameSync）：m_bucketOffsetsFrameIdx = releaseFence % 3 → Map 写帧段偏移
      DispatchCulling（Render 阶段）：门面透传 m_bucketOffsetsFrameIdx →
        COPY 源偏移 = 帧段 × (kMaxCullBuckets+1)（阴影 shadowIndirectArgs 尾部 + 主视口 gIndirectArgs 尾部）
      ```
      涉及：CullingResourceManager（容量×3）+ CullingLayer（m_bucketOffsetsFrameIdx 成员/SetFlatInstances 帧段/
      门面透传）+ CullingRenderer（DispatchCulling 签名 + COPY 两处帧段偏移）；
      **块展开能力恢复**（RenderSlotCache.cpp blockComp 分支还原——注释验证结论：块展开是实例主要来源，
      注释后实例 total=2；恢复后实例全量）；
- [x] **候选集 hash + shadowIndirectArgs readback hash（2026-08-13 用户提议）**：验证静止时输入/输出确定性——
      ①候选集 hash（Editor CullDone：coarseHash/candidatesHash 实体 id 求和）——静止时**完全恒定**
      （coarse=24/candidates=20 hash 不变）→ 分发输入确定；②shadowIndirectArgs readback hash
      （RenderDoc 抓帧时刻 CPU/GPU 变慢异常不可见——改运行时 readback hash）——阴影 dispatch 后
      COPY_SOURCE 对称屏障（UAV→COPY_SOURCE→COPY→COPY_SOURCE→INDIRECT_ARGUMENT，规则 10）+ 前部桶段区
      COPY → READBACK 堆 + 节流 120 帧 Map 求和 hash（[ShadowArgsHash][Diag]）——观察静止时阴影输出稳定性；
      ③结论：输入确定但静止仍周期闪 → 根因在下游周期因素（三帧 RingBuffer 循环/阴影 CS 桶段/块展开 Rebuild 周期）
- [x] **阴影渲染复用实体桶数据定案（2026-08-13 用户确认，修订）**：阴影渲染**不单独构建渲染项**，
      **直接复用实体构建器（Builder）的桶数据**（RenderSlotCache 桶 shaderType→Entry 指针 +
      RenderItem 队列 + 桶偏移表）——零重复构建 + 块展开共享：
      ```
      实体侧（现状，复用）：Builder 消费 RenderSlotCache 桶 → m_opaqueQueue（每桶 RenderItem）
      阴影剔除（已就绪）：光源视锥测试 → shadowIndirectArgs_光源 桶 InstanceCount（需扩展桶分段）
      阴影渲染 system（改造，不自动构建）：
        1. BeginOffscreen（DSV/视口/裁剪——复用 ShadowRenderer）
        2. 逐桶绑定：该桶 RenderItem 几何/实例段 + 阴影 PSO（默认运行在不透明阶段）
        3. ExecuteIndirect(shadowIndirectArgs_光源 桶段) —— 只画阴影可见实例
        4. EndOffscreen + 对称屏障（规则 10）
      ```
      关键认识（2026-08-13 用户修正）：**不存在 shaderType 明确指向 PSO**——Builder 消费桶时
      回调的 shaderType 参数未使用（仅做 shaderType<Count 合法性断言），PSO 由 RendererManager
      按 variantName 获取，与 shaderType 无直接映射；**阴影内容默认运行在不透明阶段**（目前所有
      实体渲染器都在不透明阶段）；**后续应基于"是否接收阴影"（receivesShadow）作为构建/筛选条件**
      （与 castsShadow 区分：投射=进阴影贴图，接收=光照阶段采样阴影贴图），而非按 shaderType 分 PSO；
- [x] **后续条件构建思考（2026-08-13 记录，未实施）**：
      - 阴影桶消费可能基于其他条件构建（如"是否接收阴影" receivesShadow 作为筛选条件——
        与 castsShadow 区分：投射=进阴影贴图，接收=光照阶段采样阴影贴图）；
      - 玻璃窗阴影：现实语义玻璃窗是有阴影的（透光但不全透），当前未考虑实现——
        远期方向：阴影贴图 alpha 衰减/半透明阴影（transparent shadow map）或
        光线透射建模（shadowViewMask 预留位承载），列入远期待决策；
- [~] **6. 场景文件阴影标记（代码四端完成，场景文件待人工）**：除地形块/树/山外组件附加 CastShadow——
      MeshDesc/MeshComponent 加 castsShadow（默认 **false，2026-08-13 用户定案：默认不投射-不接收**）+
      from_json/to_json + ParseMesh + SceneConstructor 四端已扩展（规则 23）；
      场景 JSON 现状：City4.scene.json 15487 个显式 `"receivesShadow": true`（无 castsShadow 字段）、
      City.scene.json 0 个（全依赖默认 false）；16.5MB 大场景文件的地形/树/山显式排除由人工
      按命名模式（mapChip/grs/gairoju）批量处理（AI 不批量改大文件）；
- [x] **阴影 dispatch 触发条件修复（2026-08-13）**：原 Editor 阴影收集块用 HasShadow(Directional)
      触发——其首条件 `!m_dirShadow.isValid return false`（Editor 未创建阴影贴图 → 恒 false →
      阴影 dispatch 永不发出，用户实测只发主视口 dispatch）。改为：场景存在投射阴影的方向光
      （GetLightCount(Directional) + CastShadow>0.5）即发出——阴影剔除只消费 LightViewProj 视锥 +
      填充 shadowAppend，阴影图生成是后续 GPU 驱动实例化改造（ShadowRenderer 改 ExecuteIndirect）；
- [x] **CullParams 分缓冲隔离 + 固定地址缓存（2026-08-13）**：主视口与阴影 dispatch 共享同一
      CullParams UPLOAD 缓冲时，阴影 Map 覆盖主视口参数（cullTask 0→1）→ 主视口 CS 走阴影分支
      全剔除（用户实测 u0/u1 全空）——按 cullTask 分缓冲（m_cullParamsUp / m_cullParamsShadowUp）；
      并行化基础：CreateCullingPipeline 缓存两缓冲固定 GPU 地址（GetCullParamsAddr/ShadowAddr）——
      DispatchCulling 直接绑定缓存地址，内容可预填充（FrameSync 阶段提前 Map+memcpy，Render 零 Map）；
- [x] **帧率优化（2026-08-13）**：Editor 阴影收集块加快速守卫——无投射阴影光源（方向光+聚光都无）
      时 goto 跳过（省 GetLightCount×2/GetLight 循环/frustums 栈初始化/BuildFromMatrix/Map 768B）；
      确认阴影 dispatch GPU 侧全量处理实例（instanceCount=GetInstanceCount，无 HZB 球测试应极轻）；
- [x] **CullParams 预填充 system 设计定案（2026-08-13，待实施）**：FrameSync 是主线程串行回调
      （FrameDriver.cpp:266 for 循环顺序调用），而 TaskPhase system 支持并行
      （TaskExecutor::ExecutePhase 任务图 + SystemInfo.dependencies 显式依赖）——
      将 CullParams 上传从 Render 阶段 DispatchCulling 内部（Map+memcpy ~58us+19us）改为
      **PreRender 阶段并行 system 预填充到固定地址**（Render 阶段只 SetComputeRootConstantBufferView
      绑缓存地址，零 Map）：
      ```
      PreRender（并行，ThreadType::Any）：
        System A：主视口 CullParams → m_cullParamsUp（Map+memcpy 固定地址）
        System B：阴影 CullParams → m_cullParamsShadowUp（dependencies={A} 或独立）
      Render（PrePass）：
        DispatchCulling：SetComputeRootConstantBufferView(0, 缓存地址) —— 零 Map/memcpy
      ```
      前置已就绪：分缓冲隔离 + 固定地址缓存（GetCullParamsAddr/GetCullParamsShadowAddr）；
      数据就绪：相机（Update 后）/ instanceCount（FrameSync 后）均在 PreRender 前可用；
      阶段顺序天然串行（Render 在 PreRender 之后），无竞态；
- [ ] **7. 验证**：方向光动态阴影正确（相机背后投影物不缺失/pop-in 消除）、帧率（阴影剔除无 HZB 成本极低）、
      RenderDoc 确认 shadowAppend 内容。

---

- [x] **bucketOffsetsUp RingBuffer 3→8 帧放大（2026-08-13）**：桶偏移表 RingBuffer 帧数 3→8
      （`kBucketOffsetsRingFrames = 8`，CullingResourceManager.h public）——大场景 GPU 落后 >3 帧时
      3 帧段可能被覆盖 → 桶偏移错 → CS 段基址错 → hash 变化；容量 ×8 + SetFlatInstances 取模 %8
      + DispatchCulling COPY 帧段偏移（逻辑不变，取模基数跟随）
- [x] **CullData RingBuffer 化（2026-08-13，频闪根因修复）**：m_cullDataBuffer 固定地址每帧 Map 重写
      （不滚动）——tick fence 仅限 GPU 落后 ≤3 帧，GPU 异步读时 CPU 覆盖 → 跨帧竞争（bucketOffsetsUp
      已修同类，CullData 是遗漏点）→ 阴影 CS 输出 hash 变化 → 频闪。修复：容量 ×kBucketOffsetsRingFrames
      + `CullingRenderer::Upload(frameResMgr, frameIdx)` 段滚动写入 + `CreateCullDataSRV(segmentBase)`
      （SRV FirstElement 按段偏移，@816 已支持）+ CullingLayer::Upload 门面转发 m_bucketOffsetsFrameIdx；
      `kCullDataCapacity` 移 public（C2248 修复）；
- [x] **适配器性能最高（2026-08-13）**：D3D12FeatureChecker::CreateDevice 的 -1 分支改自动选
      DedicatedVideoMemory 最大的适配器（性能最高启发：MX330 独显 > Intel Iris Xe 集显——集显压力大
      GPU 状态不稳定，阴影周期闪动嫌疑）；显式 adapterIndex ≥0 仍优先
- [x] **GridManager 崩溃诊断（2026-08-13，连带非本链路）**：GridManager::UpdateAndUpload@158 Unmap 崩溃——
      排除越界（cbSize 256 > 112B）与已释放句柄悬垂（GpuResourceManager 状态机保护）；根因在资源管理
      其他问题（连带受害者，非阴影链路/适配器切换——换卡仅 CreateDevice 选适配器，资源在新设备创建）；
      留待资源生命周期专项或单帧步进单独定位

## 七、待决策项（远期）

1. **CSM 级联**：方向光从单张正交贴图升级级联（shadowViewMask 低 16 位语义）；
2. **Clustered Shading**：光源数 >100 时引入集群着色（光源剔除独立 CS 阶段 + clusterId 索引）；
3. **阴影贴图缓存**：不动物体不每帧重渲（UE 式缓存）——`shadow.md` 中期目标；
4. **虚拟阴影贴图 / 光线追踪**：长期参考（`shadow.md` 演进方向）；
5. **阴影 = 实体派生属性 + 子网格颗粒化（材质感知阴影）**（2026-08-15 定案方向，见下 §八）

---

## 八、阴影 = 实体派生属性 + 子网格颗粒化（2026-08-15 定案：演进方向）⚠️ 桶时代方向

> **2026-08-27 注**：本节"桶段 = 材质段"（§8.2/§8.4）基于旧桶架构（每桶 IndirectArgs + 桶偏移表）
> 设计，属历史方向。子网格颗粒化（每子网格一阴影项）的思路仍有效，但恢复时需按无桶流程重设计
> （每 BatchKey 一条间接命令、子网格即命令粒度、材质由命令/段表承载）。

### 8.1 架构洞察（用户定案）

**阴影是实体的派生属性**——阴影渲染项必然对应一个实体（castShadow 实体的光源视角绘制），不是独立资产。由此：

```
当前（阴影构建器独立，2026-08-15）：
  阴影构建器每帧独立遍历 9501 条桶 + 5677 条投射实体几何解析
  （GetResource/GetGeometryBase/PickLOD）→ 7ms CPU（重复解析）

演进（实体构建器派生）：
  实体构建器【已经做了同样的几何解析】→ 在生成实体渲染项时【顺带产出】阴影渲染项
  → 阴影构建器不再重复解析，改为【消费各构建器的合并结果】（聚合器，近零计算）
```

**本质**：消除重复几何解析（实体构建器与阴影构建器对同一实体做两次 GetGeometryBase/GetResource）——不是"缓存少做"，而是"根本不做第二次"。

### 8.2 子网格颗粒化（材质感知阴影）

**阴影应下放到子网格颗粒**——不同材质的阴影不同（颜色阴影 color shadow / 半透明阴影 transparent shadow）：

| 维度 | 当前（纯深度整体绘制） | 演进（材质感知子网格颗粒） |
|:--|:--|:--|
| 阴影渲染项粒度 | 实体整体（单段 `{0, indexCount}`） | **子网格颗粒**（每材质段一项） |
| 材质语义 | 无（纯深度） | **材质感知**（颜色/半透明） |
| 桶段 | 实体主桶（单段 InstanceCount） | **材质桶段**（每材质 InstanceCount） |
| 渲染 | DirShadowVS 间接索引 + 整体区间 | 同 + **材质索引**（PS 采样颜色/衰减） |

**语义演进**：当前纯深度阴影不需要材质（整体绘制正确）；未来颜色/半透明阴影需要材质感知 → 子网格颗粒（每子网格 = 材质段 → 桶段 = 材质段）。

### 8.3 与"实体构建器派生"结合

**子网格颗粒阴影天然契合实体构建器派生**：

```
实体构建器（Worker）：遍历桶 → 实体渲染项（按材质分桶已有 subMeshRanges）
  └─ 顺带产出【子网格颗粒阴影项】：每实体每材质段 → ShadowRenderItem
     （复用 base 指针 + 材质索引，零额外几何解析）
      ↓
阴影构建器：消费各构建器的子网格阴影项合并 → 按材质桶段 + 桶编号注入 → m_shadowBucketItems
      ↓
阴影渲染：ExecuteIndirect(shadowIndirectArgs 材质桶段) + PS 材质感知（颜色/半透明采样）
```

**关键**：子网格颗粒阴影 = 阴影渲染项与实体渲染项**共享材质桶段结构**（实体按材质分桶已有）——阴影项只需"实体项 + 阴影标志"派生，材质感知天然可用。

### 8.4 设计要点（未来推进时）

1. **阴影项结构扩展**：ShadowRenderItem 加 `materialIndex` + 子网格区间（subMeshRanges 恢复材质段，而非整体单段）
2. **桶段回归材质**：shadowIndirectArgs 桶段 = 材质桶段（每材质 InstanceCount）——恢复多段（呼应当初"拆分子网格"原始设计，但为材质感知语义）
3. **剔除不变**：实体级球测试（子网格共享可见性），实例索引仍 gShadowAppend
4. **渲染 PS 材质感知**：DirShadowPS 采样材质（颜色阴影衰减 / 半透明透光）——需材质纹理绑定
5. **§10.6 铁律适配**（RenderPipelineSpecification.md）：桶编号/偏移表仍全局单点（FrameSync/CullingDataStore），子网格颗粒不改变此约束（仅桶段内容 = 材质段）
6. **当前可先整体**（纯深度阴影整体绘制已正确），架构保留演进空间：实体构建器派生阴影项时按子网格产出（材质段），后续启用材质感知只需 PS 加材质采样 + 桶段恢复材质段

---

## 参考

- 本项目：`GPU-Drive.md`（剔除分层蓝图）、`TwoPassHZB.md`（HZB 遮挡剔除）、
  `InstanceCullingSystem.md`（剔除管道）、`shadow.md`（阴影演进）、
  `SnapshotSystem.md`（GTA 查询计数器模式）
- 大型引擎：UE `FShadowDepthPass`（独立阴影视锥剔除）、Unity HDRP `LightLoop → RenderShadowMaps`
- 相关代码：`Engine/Renderer/CullingLayer/CullingResourceManager.h/.cpp`、
  `Engine/Renderer/CullingLayer/CullingDataStore.h`（CullData 80B，shadowViewMask 偏移 24B）、
  `Shaders/InstanceCulling.cs.hlsl`（gCullData t0）、`Shaders/Shadow.hlsl`（方向/点光/聚光阴影 VS/PS）、
  `Game/Game/RenderPipeline/GameRenderPipeline.cpp`（三光源阴影 System，Game.cpp:94-101 阴影贴图创建）
