# 编辑器问题清单（2026-08-04 会话归档）

> 日期：2026-08-04
> 关联：`Docs/README.md`（bugs 索引）、`Docs/architecture/rendering/StaticEntityPersistentBuffer.md`
> 状态：部分解决（见下表）

---

## 一、问题清单总览

| # | 问题 | 状态 | 备注 |
|:--|:--|:--|:--|
| 1 | **Outliner 仅第二个 Tab 显示，首个 Tab 不显示** | ✅ 已解决（绕过） | 根因：Registry::AllEntities 实现错误（view<type_list<>> 恒空）→ aliveCount 恒 0 轮询失效；绕过：改 view<NameComponent> 计数。长远需修 AllEntities（§三.1） |
| 2 | 静态实体编辑（Gizmo/属性卡）无视觉变化 | ✅ 已解决 | worldDirty 无置 true 写入点（§二.1） |
| 3 | SceneLoader::ParseEntity 未解析 persistentId（四端遗漏） | ✅ 已解决 | precomputed 永不命中（§二.2） |
| 4 | ImGui 58ms/帧（Outliner 7624 Selectable 全量渲染） | ✅ 已解决 | 虚拟列表（§二.3） |
| 5 | Builder 35ms（getComp TryGetComponent×4 为主） | ✅ 已解决 | Entry 组件指针（§二.4） |
| 6 | 静态 batch 持久缓冲变形/频闪 | ✅ 已解决 | 持久化上传为伪命题，回退（§二.5） |
| 7 | SceneTagComponent Editor 自定义组件组合 view 失效 | ⚠️ 已绕过 | 单 view + TryGetComponent（§三.1） |

---

## 二、已解决详情

### 2.1 静态实体编辑无视觉变化（worldDirty 脏标记缺失）

- **根因**：`StaticComponent.worldDirty` 全项目只有 `SceneConstructor.cpp:575` 置 false（加载时烘焙就绪），**无任何置 true 的写入点**。编辑器修改静态实体 Transform 后，Builder 判定 `entityStatic = staticComp && !staticComp->worldDirty` 恒为 true → 仍用烘焙矩阵 `cachedWorld` 渲染 → 视觉无变化。
- **修复**（两处编辑器 Transform 修改点补脏标记）：
  - `Editor/EditorLib/Viewport/Systems/EditorGizmoSystem.cpp`：Gizmo 拖动 `ImGuizmo::IsUsing()` 写回后置 `worldDirty=true + hasCachedWorldBounds=false`
  - `Editor/EditorLib/Properties/Editors/TransformEditor.cpp`：属性卡 Position/Rotation/Scale 三处 DragFloat3（值变化时）置同上 + 补 `ECS/Core/Components/Misc.h` include
- **验证**：位移可用（用户确认）✓
- **注意**：EditorGizmoSystem.cpp 不在 ECS 命名空间，`StaticComponent` 需 `DX12Engine::ECS::StaticComponent` 全限定（TransformEditor.cpp 在 `namespace DX12Engine::ECS` 内可无前缀）——踩过 C2065

### 2.2 SceneLoader::ParseEntity 未解析 persistentId

- **根因**：规则 #23 四端遗漏——`EntityDesc.persistentId` 字段/to_json 有，但 `SceneLoader::ParseEntity` 未解析 → 加载时 `eDesc.persistentId` 为空 → 静态烘焙 precomputed 按 persistentId 命中失败 → `worldDirty` 恒 true → Builder 不走烘焙矩阵（static=0）
- **修复**：`SceneLoader.cpp` ParseEntity 补 `if (j.contains("persistentId") && j["persistentId"].is_string()) entity.persistentId = ...`
- **教训**：`SceneLoader::Parse*` 是独立手工解析，新增 EntityDesc 字段必须同步 ParseEntity（四端一致性）

### 2.3 ImGui 58ms/帧（Outliner 虚拟列表）

- **根因**：OutlinerPanel 每帧 `registry->view` 遍历 7624 实体 + 每实体 `GetEntityIcon`（查组件）+ Selectable/Text 全量生成
- **修复链**（多次迭代踩坑）：
  1. 行缓存（变更驱动：`m_rowsDirty || aliveCount != m_cacheAliveCount || activeSceneId != m_cacheSceneId`）
  2. 组合 view → 单 view<NameComponent> + `TryGetComponent<SceneTagComponent>`（SceneTagComponent 是 Editor 自定义组件，组合 view 可能返回空）
  3. ImGuiListClipper + **显式行高**（`GetTextLineHeightWithSpacing`）：空 label Selectable（纯 ID `##ol_`）ItemSize 高度异常 → clipper displayEnd=1 只渲染 1 行（社区 issue #6165 同因）
  4. 实体 CRUD 轮询：每帧 `AllEntities()` 轻量计数 vs `m_cacheAliveCount`，任何路径增删都触发重建
- **效果**：ImGuiRenderSystem 58ms → 1.4ms/帧，静止/小视野 60 FPS

### 2.4 Builder 35ms（getComp 瓶颈）

- **根因**：`TryGetComponent` ×4/实体（Mesh/Transform/Static/Reflection）约 1.2-2μs/次，占 BuildTyped 70%
- **修复**：`RenderSlotCache::Entry` 缓存组件指针（Dispatch 时一次解析），Builder 热路径零 TryGetComponent
- **配套**：静态实体剔除用 `cachedWorldBounds`（首次计算缓存，跳过每帧 GetMatrix+Transform）；`StaticComponent` 增加 `cachedWorldBounds/hasCachedWorldBounds` 字段

### 2.5 静态 batch 持久缓冲 → 伪命题回退

- **结论**（用户拍板）：持久化上传是伪命题——
  1. 上传实测 0.03ms（非瓶颈）
  2. 可见集随相机变动，batch 级持久地址不稳定（staticKey 指纹每帧变）
  3. UE GPUScene 持久化的是**全场景数据**（可见性只改 Draw 引用），我们缓存的是**可视区域**（随相机漂移）——语义根本不同
- **回退**：移除 `StaticBatchCache`/`staticKey`/指纹路径，FrameSync 统一 RingBuffer
- **保留**：静态实体烘焙矩阵（cachedWorld/cachedWorldInvTranspose）+ cachedWorldBounds 省计算

---

## 三、未解决问题

### 3.1 Outliner 仅第二个 Tab 显示（首个 Tab 不显示）✅ 已解决（绕过）

- **现象**：加载第一个场景（小场景如 async_test 6 实体）Outliner 不显示；切到第二个 Tab（如 City 7624 实体）显示
- **最终根因**（增强诊断 viewCount/matchedCount/aliveCount 实证）：
  1. **`aliveCount` 恒 0**：`Registry::AllEntities()` 实现为 `m_registry.view<entt::type_list<>>()`——**空类型列表 = 恒空 view，不是"所有实体"视图** → 轮询条件 `aliveCount != m_cacheAliveCount` 永不触发
  2. 首次重建在实体加载前（activeSceneId 0→1 触发，registry 空）→ viewCount=0；实体加载后 sceneId 不变 + aliveCount 恒 0 → **永不重建** → rows 恒 0
- **绕过修复**（`OutlinerPanel.cpp`）：aliveCount 改用 `registry->view<ECS::NameComponent>()` 遍历计数（语义明确，绕开 AllEntities 错误实现）
- **⚠️ 长远修复 TODO**：`Engine/ECS/Core/Registry.h:58` 的 `AllEntities()` 实现有误——`view<entt::type_list<>>` 恒空。应改为 enTT 正确 API（`m_registry.view<entt::entity>()` 或 `m_registry.storage<entt::entity>()`），使"获取所有有效实体"语义正确。当前绕过可用，但 AllEntities 作为公共 API 语义错误是隐患（其他调用方可能踩同样的坑）
- **下一步**：若仍异常，临时恢复最小诊断日志（rows/clipper displayStart/End）定位；重点查首个 Tab 加载时机的缓存构建与 activeSceneId

### 3.2 SceneTagComponent 设计（长期）

- **现状**：Editor 端自定义组件（`Editor/EditorLib/ECS/SceneTagComponent.h`，**全局命名空间**），实体构造时加 `SceneTagComponent{sceneId}`，用于 Dispatch/Outliner 过滤 + Tab 切换保留实体
- **问题**：
  - 数据冗余（实体归属场景本由加载流程管理）
  - 每帧过滤 O(n) 遍历
  - Editor 端专属（Game 端无此概念），组合 view 失效（§2.3 已绕过但暴露耦合）
- **长远建议**：per-scene 实体容器（SceneManager 按 sceneId 分组持有实体集合），过滤 O(1)，SceneTagComponent 可退役；场景切换时长当前不受影响（AddComponent 占比小）

---

## 四、性能排查历程要点（存档）

- 帧率瓶颈链：Builder 35ms（getComp）→ ImGui 58ms（Outliner）→ 主循环 frame≈tick（FrameDriver 内部）→ renderSys（ImGui 为主）→ 场景内容无关的 60ms 固定开销 → Outliner 7624 Selectable
- 大型引擎对照：UE GPUScene（全场景注册 + 可见性只改 Draw 引用）、UE FMeshDrawCommand（静态命令预构建）、ImGuiListClipper（官方虚拟列表，label 必须含可见文本）
- 每实体 6μs 的真相：getComp（TryGetComponent 哈希）+ batch 组装（unordered_map/vector realloc）

---

## 五、PVS 预遮挡：实施与清理（2026-08-04）

> 状态：**已清理（2026-08-04 代码全部移除）**——参考大型引擎 id Tech 经典 PVS（区块→可见集、静态专用、动态解耦）实施，因暴露问题放弃并清理代码。

### 曾实施（2026-08-04 移除）
1. **数据结构**：`PrecomputedPVS`（区块 → 可见 persistentId）+ `SceneDescription.h` pvs 字段 + 序列化
2. **生成**：`ExportToDescription §7`（save 时遮挡测试：区块中心→实体中心射线 slab AABB；距离预筛排序 + 命中即 break 优化 O(n²)→秒级；真实 worldBounds = MeshComponent.localBounds × transform；large 大物体跳过遮挡）
3. **运行时**：`OnSceneConstructReady` 存 `m_pvs` + `OctreeCulling::ApplyPVSFilter`（相机区块查表过滤被遮挡实体）
4. **PVS 独立文件**：`{filePath}.pvs.json`（scene.json 不含 pvs——Save 分离 + AssetBrowser Load 附加）

### 放弃与清理原因
1. **scene.json 膨胀**：pvs 50 blocks × 平均 3273 可见引用 = **16.4 万条** → scene.json 5MB → **25MB**（解析/save/用户 json 工具全慢）
2. **可见率 43%**：遮挡测试太宽松（单中心射线），过滤效果差（pvs 表大但剔除少）
3. **大量地块被误删**：小块地块（mapChip03/mapParts 等）被遮挡测试误判"被遮挡"→ 运行时剔除 → 地块丢失
4. 加载/save 时长问题（pvs 生成 + 解析开销）

### 若未来重做（改进方向 TODO）
1. 遮挡测试保守化：只剔确定被遮挡的**小物体**（树/路灯），地块/大物体排除
2. 可见率优化：多方向采样/射线密度提升（剔除更准，表更小）
3. pvs 文件压缩/紧凑格式（减少 16 万引用体积）
4. 加载异步化（pvs 后台解析，不阻塞主循环）

---

## 六、当前剔除/优化架构定稿（2026-08-04）

```
空间哈希（OctreeSystem 重写）：格子 250、实体入覆盖格子、格子级视锥剪枝、GTA 计数器去重（uint32，独立 stamp/查询）
  → 视锥剔除（FrustumCullAABB 15% 扩展）
  → 粗筛层 cullDistance 拒远（MPD @CullFar，coarse -84%：5367→890）
  → Dispatch（RenderSlotCache，组件指针缓存）
  → Builder 4 分块并行（依赖图 Worker 线程池）+ MergeChunk（queueIndex/tempSlot 修正）→ 0.09ms
  → 相机静止缓存（ViewProj 未变复用 coarse）
帧率：小视野 60-109 FPS / 中等 32-56 / 大视野 26-54
```

- 关键数字：粗筛层剔除后 coarse 峰值 5367→890（-84%）；Builder 7-35ms→0.09ms（4 分块并行）；剔除链路核心开销归零（builder/cellsHit 非瓶颈）
- 剩余瓶颈：coarse（实体量，距离剔除后 300-890）——PVS 是后续候选（暂缓）

---

## 七、当前问题清单（2026-08-04，除天空盒外）

> 天空盒 `Sky not found` 单独记录（Content/City/Textures/Sky.dds 缺失，走天空盒兼容映射，不入此清单）。

### ✅ 已解决
1. **culling 剔除瓶颈**：12-20ms（占 frame 50-70%）→ **0.08-0.55ms**——注册 system 并行剔除（4 chunk 依赖图并行 + 合并，动态块数按硬件线程）+ 视距 2000 + 锥形预筛。剔除不再是帧率瓶颈。
2. **cullDistance 丢失**：ExportToDescription 四端遗漏（导出未写 cullDistance → save 后全丢）→ 已补（tDesc.cullDistance）。
3. **PVS 误删地块/膨胀**：已放弃清理（§五）。

### ⚠️ 待处理
1. **JSON 膨胀**：scene.json ~10MB（cullDistance/precomputed 恢复后仍大）——失去 JSON 意义，方向：**SOA 布局二进制内容块**（体积减少，转换工具输出）。
2. **MPD 方向光/雾/天空盒未解析**：MPD 文本标记区含 `LightDir(x,y,z)`/`LightColor(r,g,b)`/`FogColor(r,g,b)`/`SkyXFile`（05_MPD_Format_Analysis.md §1.4）——MapSceneConverter 未解析（只解析 @CullFar/@AlphaTestFlag）——Sun 方向光目前手动补，应改转换器从 MPD 提取。
3. **cellsHit 统计缺失**：`QueryFrustumChunk` 未更新 `m_lastCellsHit`（分块查询后 cellsHit 恒 0）——仅统计字段，可补。
4. **区块化（chunk）待做**：MPD 区块划分是原始资产问题，可**自动/手动划分**区块化内容（用户确认方向）——让用户感觉视野开阔（超大规模场景时 CPU 遍历/内容量）。

### 🔭 方向（用户定案）
- **区块化**：chunk 必要——自动/手动划分区块化内容（不依赖 MPD 原始区块）
- **二进制 SOA**：JSON 太大 → SOA 布局二进制内容块（体积减少，转换工具改造）
- **MPD 信息提取**：转换器解析 LightDir/LightColor/FogColor/SkyXFile（方向光/雾/天空盒从 MPD 提取，替代手动补）

---

## 八、集群语义定案 + ECS 量级 + 聚合形态（2026-08-04）

### 集群语义（用户定案）
- **集群不代表不会遮挡**：建筑群太高——**远近裁剪面不适合大内容**（大包围盒内容不适用远近裁剪）
- **预遮挡（PVS）适用**：山可以遮挡建筑群——**PVS 优先于集群包围盒**（预遮挡计算优先）
- **山不应被远裁剔除**：山是地形边界（像天空盒一样的部分）——地形边界内容不适用远近裁剪

### ECS 组件遍历/get 性能量级（实测参考）
| 操作 | 成本 | 量级判断 |
|:--|:--|:--|
| **遍历（view 迭代）** | O(n) 迭代器（enTT 批处理/SIMD 友好）| 数万-数十万实体可接受（<10ms）|
| **TryGetComponent**（逐实体 get）| ~50-200ns/次 | **数万实体开始瓶颈**（实测 7624×4 = 1436-5165μs、Outliner 58ms——组件指针缓存后 83-274μs/1.4ms）|
| 综合 | - | <1 万正常；1-5 万需**缓存组件指针**；**>5 万需聚合/分区** |

**结论**：ECS 遍历（view）不是瓶颈（数万-数十万 OK）；**逐实体 TryGetComponent 是瓶颈**（数万开始显著——之前 getComp/Outliner 教训已用组件指针缓存解决）。

### BlockComponent 聚合形态（减少 ECS 常态实体数）
- **动机**：ECS 常态实体数过多（City 7624）——遍历/get/管理开销随实体数增长——**将多个实体聚合为 chunk**（少数组块实体）——**ECS 常态实体数 7624 → 几十（-99%）**
- **收益**：遍历/get/管理开销降、剔除/渲染以 chunk 为单位（对齐"超视距不需要剔除太多内容"）
- **权衡**：聚合牺牲剔除精度（chunk 内不可见内容也处理）——**PVS/遮挡计算补偿**（用户观点：PVS 优先——山遮挡建筑群）
- **定位**：BlockComponent 的**另一种形态**（chunk 聚合——将实体聚合为块实体）——区别于集群（大包围盒可见性）

#### 全图分块实测（2026-08-05，10 张地图，500 单位）

| 地图 | 对象数 | 非空格 | 唯一 piece | 世界范围 | 500块数 | 1000块数 |
|:--|:--:|:--:|:--:|:--|:--:|:--:|
| City | 7623 | 604 | 40 | 810×750 | 4 | 1 |
| City2 | 7594 | 604 | 40 | 810×750 | 4 | 1 |
| City3 | 3369 | 152 | 33 | 763×763 | 4 | 1 |
| City4 | 15618 | 902 | 28 | 1050×858 | **5** | 2 |
| City5 | 9850 | 601 | 30 | 834×720 | 4 | 1 |
| City_tac | 9191 | 1157 | 29 | 2100×2070 | **36** | 9 |
| In | 896 | 324 | 15 | 510×510 | 4 | 1 |
| In2 | 350 | 53 | 19 | 210×210 | 4 | 1 |
| moon | 291 | 97 | 109 | 270×270 | 4 | 1 |
| skyland | 6552 | 704 | 72 | 750×884 | 4 | 1 |

**块大小定案（2026-08-05）**：500 单位（1000 单位 9/10 图退化 1 块，块级剔除失效；500 与 Phase C 现有实现一致）。
**流式结论**：类 EXVS 竞技场不做流式（无跨区域漫游，整图一次加载即可）——详见 `Docs/targets/UKW_PowerUpKit/08_MapScenePipeline.md` §八/§九。
分析脚本：`.atomcode/tmp/mpd_blocks_analyze.js`。

---

## 九、资产格式迭代策略（2026-08-04，用户定案）

- **JSON 与二进制并行支持**：JSON 格式用于**快速迭代系统和测试字段**；二进制（SOA `.scene.bin`）用于**大量内容**（固化/发布）
- **迭代方向**：**先 JSON 验证，后二进制固化**——新系统/新字段先用 JSON 快速迭代验证，稳定后再转为二进制固化
- **引擎双加载**：`SceneLoader::LoadFromFile`（JSON——迭代）与 `LoadFromFileBinary`（SCNB 二进制——固化）并行支持
- **定位**：Phase B（SOA 二进制）是**固化层**（体积 9%、加载加速），JSON 是**迭代层**（可读/可测）——两者互补，不互相替代

---

## 十、水渲染设计定案（2026-08-04，用户定案）

### 数据流（水渲染接入）
- **lightCBAddr**：直接走光照数据（LightingRenderer 光照 CB）
- **envMapSRV**：水应用环境反射贴图——**暂时用天空盒的立方体贴图数据**（SkyboxManager 环境贴图 → WaterManager.SetEnvironmentMap）

### 水的特殊性（程序化水面）
- **MPD 是否具备"将邻接的一组区块作为水"的能力待确认**——若存在：**水事实上不存在网格**——使用**程序化四边形**——这就是"一块水"的含义
- **"一块水"语义**：邻接的水是**整体**（物理上正常）——可程序化，或 **MPD 解析后给出区块水的角点**——在世界空间知晓水的范围四边形
- **Game 端以前**：水是一整块——利用**叠加 + 深度检测**完成视觉正确（水与地形深度叠加，深度边缘正确）

### 参考大型引擎（待对照）
- 大型引擎的水：程序化水面（平面四边形 + 波浪（Gerstner）+ 深度边缘检测 + 环境反射）——对齐方向

### 实施路径（水渲染接入，任务 2 待续）
1. WaterManager 驱动（CollectFromECS/UpdateAndUpload——waterCB 上传）
2. 渲染帧水绘制（WaterRenderer.BeginFrame/DrawWater——程序化四边形 + 天空盒 envMap + lightCB 光照）
3. MPD 区块水角点确认（或程序化四边形范围）
