# 全局待办清单

> 日期：2026-08-02（上次更新 2026-07-27）
> 本次会话（2026-08-02）：**AssetManager 注册表模式完成**（#8 闭环）。以文件后缀分发替代 `switch(type)`（Godot ResourceFormatLoader 模式，见 `AssetLoaderImprovement.md` 定案）；`.anim` 保留 `LoadAnimation` 专用入口；按资产类型拆文件（`Loaders/` 目录 5 个 `Register*Loader`）；移除旧 `Load(path, type)` 签名无过渡；Game 端搁置仅记录变更点。编译通过 + 运行时无异常。本次同时清理了待办清单（标记完成/去重/归档过时条目）与旧快照归档。
>
> 2026-08-01 会话：**UKW 资产管线定案：FBX 为引擎资产唯一来源**。DCC 侧定案：骨骼视觉断开为数据固有（部件变换矩阵语义，非关节链几何），接受现状；唯一有效改动是 FBX 导出改二进制（Blender 5.2 不支持 ASCII）。**第二次定案**：importrobot（.x 直接拼接）退役不调用，仅留作参考；引擎资产只从 Blender 优化后的最终 FBX 产出（`fbxs2dxmesh` 转换器立项 P1）；武器/挂点不建骨骼，走 socket 挂载模式；ANI 直解也转 FBX 而非引擎资产。详见 `Docs/targets/UKW_PowerUpKit/07_EngineAssetPipeline.md`。
>
> 详细快照见 `Docs/snapshots/archived/WorldRefactor_Snapshot_20260723.md`。
>
> **重要设计决策**：
> 1. 编辑器端采用标记组件方案（`SceneTagComponent`），所有场景实体共存于同一 ECS Registry，通过 `sceneId` 区分。切换 Tab 时不动 Registry，只更新活跃索引，Builder 按 `sceneId` 过滤。详见 `Docs/architecture/scene/SceneManager.md §10`。
> 2. 属性卡采用 ECS 组件驱动注册制：`ComponentEditorRegistry::Register<T>(drawFn)`，每个组件类型独立注册编辑方法，控制逻辑分散到各组件。ImGuizmo 集成用于 3D 变换操作。详见 `Docs/architecture/editor/ComponentEditorSystem.md`。
> 3. 输入系统改为声明式推送模式：`InputSystem::BindCallback()` 注册回调，`InputSystem::Update()` 末尾自动触发。System 在注册时通过 `SystemInfo::inputDeclarations` 声明输入需求。详见 `Docs/architecture/core/InputSystem.md`。
> 4. **SceneManager 不是所有 ECS 实体的源头，ECS Registry 才是。** SceneManager 只做场景实体的 CRUD（增删查不改），组件值修改由各 System 自行负责。选中实体独立为 SelectionService，不依赖 SceneManager。详见 `Docs/architecture/scene/SceneManager.md §11`、`Docs/architecture/editor/ViewportToolbar.md §十二`。

---

## 二、待办

### 可恢复的功能

| # | 任务 | 优先级 | 说明 |
|:-:|:-----|:-------|:------|
| 1 | 公告牌手动放置（JSON 驱动） | P2 | 已有 `BillboardDesc`/`BillboardComponent`/Builder，只需恢复注册 + 接入 |
| 2 | 公告牌程序化体积生成 | P3 | `BillboardVolumeDesc` + CPU 生成，见 `BillboardSystemArchitecture.md` |

### 动画管线（2026-07-31 定案）

| # | 任务 | 优先级 | 说明 |
|:-:|:-----|:-------|:------|
| 3a | **ANI 解析器** | P1 | ✅ 已完成（2026-07-31）：`ANIParser` 读 `Script.ani`（文件名头 + HOD/HD2 块序列）→ 按组提取帧矩阵 + Tail 状态机。1.008 原版（HOD）与 PUK 2.008（AN2+HD2）双格式，标记法 + 母版驱动，无固定步长，25 机体全量拆解成功。**下一步：B2 `.anim` 现代化资产（全量转切分剪辑 + AnimLoader）**。详见 `Docs/architecture/assets/CharacterAsset.md` §九 阶段 B1/B2 |

### UKW 引擎侧管线（2026-08-01 定案：FBX 唯一来源）

> 路线定案：**引擎资产只从 Blender 优化后的最终 FBX 产出**。importrobot（.x 直接拼接）退役不调用，仅作参考实现。武器/挂点不建骨骼，走 socket 挂载模式（`RelationshipModel.md`）。ANI 直解（ani2anim）只作进 Blender 的桥，不直接产引擎资产。详见 `Docs/snapshots/UKW_AssetPipeline_Snapshot_20260801.md`、`Docs/targets/UKW_PowerUpKit/07_EngineAssetPipeline.md`。

| # | 任务 | 优先级 | 说明 |
|:-:|:-----|:-------|:------|
| U1 | **fbxs2dxmesh 转换器** | P1 | ✅ 已完成（见 `07_EngineAssetPipeline.md` §五）：最终 FBX（Blender 优化后）→ `.dxmesh` + `.bone` + `.mat` + `scene.json`；kd-03 实测 8 子网格/3045 顶点/15 骨骼。设计要点：子网格按材质槽拆（合并材质后 38 → 5~6）；骨骼索引从顶点权重读（不靠文件名推断）；`.bone` 以 FBX 解析结果为准（去 `_bone` 后缀、过滤 `_end` 末端节点）；Blender Principled BSDF → 引擎 .mat；Y-up 右手系 → 左手 Y-up 翻转 Z（复用 importrobot 规则）。CLI：`fbxs2dxmesh <model.fbx> <output_dir>` |
| U2 | **SkeletonManager 加载 `.bone`** | P1 | ✅ 已完成（见 `07_EngineAssetPipeline.md` §五）：`LoadFromJSON` 已实现（读 bones 数组：name/parentIndex/position/rotation/scale），`SkeletonLoadTask` 异步路径就绪。引擎侧从 `.bone` JSON 加载骨架树 |
| U3 | **SkinnedComponent 接入** | P1 | 场景加载时创建 SkinnedComponent（骨架引用 + 网格引用），`SceneConstructor` 框架已有 |
| U4 | **蒙皮渲染链路** | P1 | GPU 骨骼缓冲（StructuredBuffer<float4x4>）+ 蒙皮着色器绑定（`DxMeshSkinnedVertex` 的 R8G8B8A8_UINT boneIndices + boneWeights） |
| U5 | **动画资产 B2（.anim 现代化）** | P2 | 从 Blender 动画 FBX 切分剪辑 + AnimLoader（`CharacterAsset.md` §九）；ANI 直解仅作桥，不直接产引擎资产 |
| U6 | **IK 入引擎** | P2 | IKSolver 已离线验证（B 方案），引擎侧接 FABRIK 需先清蒙皮管线三段缺口（U2-U4） |
| U7 | **socket 挂载消费（三层模型）** | P2 | 武器/特效点等挂载实体经 `RelationshipComponent(kind=socket)` 挂到角色骨骼，**不建骨骼**（详见 `RelationshipModel.md` §四）。分三层落地：① **socket 跟随**——父骨骼世界矩阵（来自蒙皮链路 GPU 骨骼缓冲）驱动挂载实体 Transform；② **状态驱动偏移**——不同动作状态覆写 socketOffset（如盾防御姿态对准正面，解法 A）；③ **挂载实体自身动画**（可选，后期）——SocketAnimComponent 播局部剪辑叠加于父骨骼之上（盾姿态/武器后坐力，解法 B）。最终变换 = parentBoneWorld × localAnim × socketOffset(state) |
| U8 | **武器挂载动画数据提取** | P2 | 从 ANI 帧局部矩阵提取 gun/sword/Shield 部件通道 → 武器自身 .anim 剪辑（U7 第③层 SocketAnimComponent 的数据源）。依据：ANI 帧 = HOD 块序列，每帧存**部件局部 4×4 矩阵**（`02_RobotAndAnimation.md` §2.1），武器局部矩阵可直接作为挂载实体自身层动画，零转换零丢失。与 U5（机体动画 B2）同源（ANI 帧），仅切分维度不同（机体骨骼通道 vs 武器部件通道） |

### 基础设施改进

| # | 任务 | 优先级 | 说明 |
|:-:|:-----|:-------|:------|
| 4 | `DxMeshLoader` 完整实现 | P1 | 当前 `MeshLoadTask` 已读 `.dxmesh`，但需完善骨骼数据路径 |
| 5 | **组件驱动属性卡** | P1 | 基于 `ComponentEditorRegistry` 注册制的属性卡系统，替代当前硬编码的 Properties 面板。<br>• ✅ `ComponentEditorRegistry` 注册器（模板 Register<T> + 回调存储）<br>• ✅ `EditorLayout::DrawProperties()` 已重构为遍历注册表 + 按组件类型折叠 + 添加/移除按钮<br>• ✅ `RegisterTransformEditor()` 已注册（Position/Rotation/Scale 数值输入 + 四元数↔欧拉角转换）<br>• ✅ `RegisterLightEditor()` 已注册（类型/颜色/强度/范围/阴影参数）<br>• ✅ "添加组件" 弹出菜单（只显示实体上不存在的组件）<br>• ✅ "移除组件" 按钮（Transform 等核心组件不可移除）<br>• ✅ 编辑器 UI 标签已接入 `EditorStrings::Get()`（TransformEditor/LightEditor 均已迁移）<br>• ✅ `CameraComponent` 已创建（ECS 结构体 + SceneConstructor 创建 + CameraEditor 注册 + ExportToDescription 导出）<br>• ⚠️ `CameraComponent` 属于 **Scene Entity Data** 层，非引擎 CORE。后续设计讨论：投影类型（Perspective/Orthographic）、PiP 预览、视锥 Gizmo |
| 7 | 多堆域 SRV 隔离（SkyboxManager → LightManager 等） | P2 | SkyboxManager 已支持 `HeapTag` 参数，SRV 创建在指定堆域。LightManager、ReflectionProbeManager 等需同样扩展，使其在多堆（Editor）模式下不占用 Default 堆域空间 |
| 8 | AssetManager 注册表模式 | P1 | ✅ 已完成（2026-08-02，编译通过 + 运行时无异常）：见 `AssetLoaderImprovement.md`（已定案），Godot ResourceFormatLoader 模式，用文件后缀分发替代 `switch(type)`；`.anim` 保留 `LoadAnimation` 专用入口（anim 与 bone 以骨骼名紧密耦合）；移除旧 `Load(path, type)` 签名无过渡；Game 端搁置仅记录变更点；Character 聚合独立化联动快照阶段 3；✅ 已按资产类型拆文件（`Loaders/` 目录 5 个 `Register*Loader`，参照属性卡注册制，§3.7） |
| 9 | TerrainLoadTask 拆分为并行子 Task | P1 | 几何体 + 纹理各走独立 Task |
| 10 | **渲染管线清除职责分离** | P2 | 当前清除散落在各渲染 System 内部，导致重复清除和职责混杂。重构方向：<br>• **Editor**: 注册独立 `EditorClearSystem`（`alwaysRun`，`RenderPhase::PrePass`），负责清除 EditorViewport 的离屏 RT + depth，设置视口。`EditorSkyboxRenderSystem` 和 `EditorGridRenderSystem` 只渲染、不清除。<br>• **Game**: GameWorld 的 PrePass（清 backbuffer+depth）、GBuffer（清 G-buffer）、Skybox（清 backbuffer+depth）存在多次重复清除，需统一到 PrePass 阶段的一个 ClearSystem。<br>• Game 和 Editor 是独立 exe，互不影响，各自按需实现。见 `BugFix_EditorViewport_ClearValueMismatch.md` 和相关讨论 |
| 11 | **Bootstrap 职责分离：不初始化非 Default 堆域分区** | P2 | 当前 `Bootstrap::InitializeModules()` 在 `isEditor && Multi` 模式下为所有非 Default `HeapTag` 创建了 `Texture/Buffer/Shadow` 分区。但 Bootstrap 属于引擎核心层，不应感知编辑器特定的堆域布局。正确设计：<br>• Bootstrap 只负责选择 `HeapMode::Multi`（基于 `ProjectConfig::Type`）<br>• 非 Default 堆域的分区由各 Editor 模块（如 `EditorViewport`）在自身初始化时注册<br>• 当前为临时方案，后续需将 Bootstrap 中 `isEditor && Multi` 块内的 `AddPartition` 循环移出，交由 Editor 各模块自行管理 |
| 12 | **编辑器布局重构** | P2 | 当前 `EditorLayout` 直接管理所有面板的创建和绘制，存在耦合。改造方向：<br>• 布局只作为大体分块框架：左列（Outliner/AssetBrowser）、中列（Viewport）、右列（Properties）<br>• 每列再分为上下两部分 |
| 14 | **网格比例尺控件** | P2 | 当前水平滑条不符合习惯，改为垂直刻度尺样式 |
| 15 | **RenderScene::OnScenePreUnload 驱动** | P2 | 场景切换时调用 LightManager::Clear() 等，当前为骨架实现 |
| 16 | **Undo/Redo 系统** | P2 | EditorSceneManager 的 EntityDesc 编辑历史（菜单项已注册，实现为空壳） |
| 17 | **BackBuffer barrier StateBefore 写死 PRESENT（GBV #942，偶发）** | P3 | `EditorMainClearSystem`（Editor.cpp）barrier `StateBefore = D3D12_RESOURCE_STATE_PRESENT` 写死，但首帧/OnResize 后从未 Present 的 backbuffer 实际状态为 **COMMON**（数值同为 0x0 但 D3D12 区分逻辑状态）→ GBV #942 `Incompatible resource state`。**不总是触发**（仅首帧/回退 buffer 首次使用时）。2026-08-09 记录：暂不修复，**待 RDG（渲染依赖图）推进后由 RDG 统一管理资源状态转换自然消除**——手写状态假设正是 RDG 要消除的模式 |

### 新设计方向（2026-07-27 讨论定案）

| # | 任务 | 优先级 | 说明 |
|:-:|:-----|:-------|:------|
| 17 | **CameraManager 从场景 CameraComponent 初始化** | P2 | 场景加载后，从 `isMain=true` 的 CameraComponent 读取 FOV/近远面，替代当前硬编码配置。Game/Editor 端各自由 Gameplay 脚本决定相机位置 |
| 20 | **Editor 端多选联动** | P3 | 拖拽父实体时，临时查 `RelationshipComponent.kind==parent` 的子实体，同步位移。不在引擎 CORE 中实现 |
| 21 | **Outliner 实体创建（实体模板）** | P2 | Outliner 右键弹出 Godot 风格创建菜单（分类+搜索）。ECS 组件组合由 `Editor/Config/entity_templates.json` 定义，JSON 驱动。模板如 Camera = Transform + CameraComponent，Empty = Transform 等。不暴露原子 ECS 组件给用户 |
| 60 | **场景状态机与实体生命周期** | P3 | 📋 已定案（2026-08-02，见 `SceneStateMachine.md`）：场景=扁平纯容器（不含类型/切换/输入/角色）；类型不可枚举（编辑器 JSON 定义，引擎 CORE 只读）；独立全局状态机文件（节点=场景，边=加载语义 replace/additive/stream）；输入上下文栈（渲染叠加，非栈式场景）；角色跨场景持久实体层（player 无 sceneId）；编辑器引用式/Game 扁平式双形态。实现留待场景切换系统立项时细化 |
| 61 | **渲染器数据驱动与绑定架构** | P3 | 📋 已定案（2026-08-02，见 `RendererDataDriven.md`）：渲染器差异=PSO 集合（几何条件共享+着色器变体各一）；静态描述/动态绑定分离；BeginFrame(pass)/Draw(item) 拆分；RenderContext 外观（get 透传管理器+占位兜底）；几何/材质条件校验（UI 源头过滤+构建器兜底）；特殊渲染器（阴影/天空盒）保留管理器；大型引擎（UE FMeshDrawCommand/Unity SRP）比对确认可行性。**材质→渲染器路由定案（§4.1）**：材质 shaderType 决定渲染路径（替代组件标志模式），子网格展开（几何切分）与材质分发（渲染路径选择）为**正交维度**，不冲突。**缓存分桶定案（2026-08-03，§4.1b/c/d，取代 §4.1a 材质组件化）**：通用 `RenderSlotComponent`（`Slot.shaderType`=渲染器标记）+ `RenderSlotCache`（缓存表实体→槽位 CRUD 驱动重建、桶每帧由 CulledSet×缓存表派生，无 visible 标志）+ CulledSet 八叉树粗筛分发 + Builder 消费桶（精筛保留）。✅ **已落地（2026-08-03）**：RenderSlotComponent/RenderSlotCache/CRUD 点 MarkDirty/Editor 与 Game 接线/三消费者迁移。**渲染项统一定案（2026-08-03，§4.2a1）**：全可能大渲染项 `RenderItem : RenderItemCommon`（可选字段=JSON 可选部分，组件存续决定用哪些，无效即不绑定）+ bindings 模式（UE FMeshDrawShaderBindings 同款）；组合约束从编译期类型隔离移到注册期 `RendererPairing`（渲染项↔渲染器↔消费槽位清单）。**槽位全声明（2026-08-03，§2.2）**：槽位集=渲染项字段集，renderer.json `rootSignature.params` 全声明可能槽位（不限于 PassConstants：LightConstants/WaterCB/BoneBuffer…）；字段→绑定形态三分类（资源句柄→DescriptorTable/RootSRV/CBV、结构化小数据→CBV、单标量→**Constants32 根常量**）。**推进路线图（§6）**：L0 绑定槽位 enum + PSO schema（✅ 已完成：BindSlot.h + renderer.schema.json）→ L1 材质路由（✅ 已完成：ShaderRoute.h + Opaque/Skinned Builder 分发）→ L1.5 缓存分桶（✅ 已完成 2026-08-03）→ L2 试点数据化（✅ 大部分完成：PSOFactory/BindSlot/渲染项自包含/根签名 JSON 化；**StateCache 暂缓**——渲染器↔system 一对一独占命令列表，多 PSO 顺序执行待 RDG 阶段，§3.4 标注）→ **L2.5 渲染项统一 + JSON 槽位全声明（Step 4.5，定案待实施：渲染项统一 + Builder 组件推断 + `ApplyBindList` 补 `Constants32` 实现 + RendererPairing 扩展）** → L3 横向铺开 → L4 RenderContext + 热重载。前置（#22/#23/#24 材质槽 + 子网格统一语义）已于 2026-08-02 完成。**RDG 定位（§7.11）：不设集中式 RDG，思想分散化——编译分层、缓存属地；场景级缓存（PSO 集合/PVS/烘焙光照）归全局状态机（`SceneStateMachine.md`）管理，帧级（屏障/剔除/LOD）归 FrameDriver，进程级归各管理器**。实现按 §6 步骤推进，联动材质槽模式（#22/#23/#24）。快照：`Docs/snapshots/RendererDataDriven_Snapshot_20260803.md` |

### ECS 统一数据源 + Manager 打包器设计（2026-07-27 补充，2026-07-28 更新）

详见 `Docs/architecture/core/EngineOverview.md §9`：

- ECS Registry 是运行时唯一数据源，Manager **不持有私有数据**，只做 ECS → GPU buffer 的聚合打包
- 需要剔除/拾取/属性卡编辑的数据（灯、相机、水面）必须在 ECS 中有对应的 Component
- Manager 存在的唯一理由：多个实体的数据需要聚合为连续 GPU buffer，或管理 GPU 资源（shadow map、纹理数组）
- 过渡路径：CameraComponent → LightManager → WaterManager 逐步迁移
- 场景 JSON 统一由 SceneConstructor 写入 ECS 组件，Manager 不再接收直接注册调用

**Manager 收集模式实施状态**（CameraManager ✅ 已有、LightManager ✅ 已实施、WaterManager ✅ 已实施），详见 `Docs/architecture/core/EngineOverview.md §9`。

**SceneConstructor 变更**：
- `LightComponent` 创建从 TODO 占位变为完整实现，支持 directional/point/spot 三种类型
- `WaterComponent` 波浪参数直接写入 ECS（amplitude/frequency/speed/direction），不再调用 `WaterManager::RegisterWaveParams`

#### SceneEnvironment 语义边界（2026-07-28 补充）

并非所有场景数据都能放入 ECS 实体。详见 `Docs/architecture/core/EngineOverview.md §9.6`：

- **T 恤线**：有 `TransformComponent`（位置/变换）的数据 → ECS 实体 `entities[]`；无实体身份的全局参数 → `sceneEnvironment`
- **`sceneEnvironment`** 分组存放管理器特有全局数据：环境光（ambient）、天空盒（skybox），不进入 ECS Registry
- **`entities[]`** 存放 ECS 实体数据（灯、相机、水面、网格物体等），Manager 通过 ECS view 按需收集
- JSON 格式：`sceneEnvironment.ambient` / `sceneEnvironment.skybox` 替代顶层的 `environment` / `skybox`
- 此语义边界已在 `SceneDescription.h` 的结构体定义中实现，是场景 JSON 文件格式的一部分

### 实体模板设计要点

- 不暴露原子 ECS 组件给用户（Cocos/Godot 模式下用户操作的是组合结果）
- 模板定义在 JSON 中（`Editor/Config/entity_templates.json`），无需改 C++ 代码即可扩展
- 新建实体时 Outliner 弹出 Godot 风格弹窗（分类树 + 搜索框）
- 模板 JSON 文档见 `Docs/architecture/scene/EntityTemplates.md`

#### Outliner 实体模板弹窗遗留问题

| # | 问题 | 说明 |
|:-:|:-----|:------|
| E1 | **Water 模板缺少 WaterManager 注册** | `CreateEntityFromTemplate` 仅添加了 `WaterComponent` 标记，未调用 `WaterManager::RegisterWaveParams`。当前已改为 ECS 组件存储波浪参数（amplitude/frequency/speed/direction），需确认模板 JSON 中的 `water` 字段能否被 `OutlinerPanel::CreateEntityFromTemplate` 正确读取并写入组件 |
| E2 | **Mesh 模板未实现** | `CreateEntityFromTemplate` 中 mesh 分支被注释（`// mesh 暂不实现（需要从 AssetBrowser 选择具体网格/材质）`）。模板 JSON 中的 `mesh.geometry` / `mesh.material` 为空字符串，创建后无法渲染。后续需实现：① 模板创建时添加占位 MeshComponent；② 用户通过 AssetBrowser 拖拽网格/材质到实体上覆盖 |
| E3 | **缺少文档** | `Docs/architecture/scene/EntityTemplates.md` 尚未创建，模板 JSON schema 和规则无文档说明 |

### 关系模型的约束

详见 `Docs/architecture/scene/RelationshipModel.md`：

- 场景 JSON 保持扁平，关系通过 `persistentId`（fnv1a 64-bit hash 字符串）引用表达
- **运行时直接存储 `entt::entity` handle**，O(1) 访问。SceneConstructor 加载时两遍解析：首遍建 hash→entt::entity 映射，次遍 resolve targetId
- 导出时从 `SceneSnapshot::entityDescs` 缓存读取 hash，**不需要从 ECS 反查**
- `entt::entity` 自带 generation，目标销毁后 handle 自动 invalid，不存在"查到错误内容"的问题
- 引擎 CORE 只存储 `RelationshipComponent`，不做自动 Transform 传播、不做级联删除、不维护树缓存
- 骨架层级（骨骼蒙皮）不是场景关系，属于动画系统
- Game 端不为 Editor 端的便利买单——Editor 需要时自行建临时索引

### 共享数据中心

| # | 任务 | 优先级 | 说明 |
|:-:|:-----|:-------|:------|
| 55 | **DataSlotPool 多池路由** | P2 | 当前 `poolId` 已编码在 `DataSlotHandle` 中（4 bits，支持 16 池），但实际所有调用都传 `0`。后续应根据数据类型（Mesh/Texture/Audio 等）使用不同 `poolId` 分配到不同池 |

### 预览系统

| # | 任务 | 优先级 | 说明 |
|:-:|:-----|:-------|:------|
| 56 | **缩略图磁盘缓存** | P2 | 当前缩略图仅存在于运行时内存（ThumbnailArray），关闭后丢失。需要：<br>• `BackgroundExecutor` 异步执行 `ReadbackSlice` + 写入磁盘<br>• 缓存文件使用独立后缀（如 `.thumb`），**禁止使用 `.dds`**<br>• 缓存目录：`Content/Cache/Thumbnails/`<br>• 启动时从磁盘加载缓存到 ThumbnailArray |
| 57 | **预览属性卡增强** | P3 | 当前已有 Orbit 相机控制（拖拽旋转 + 滚轮缩放）和光照参数滑块，缺：<br>• 程序化模型切换（球体/立方体/环面）<br>• 材质参数调整（颜色、粗糙度、金属度等） |
| 18 | **纹理预览兼容性** | P2 | 排查并补充预览系统支持的纹理格式列表 |
| 19 | **缩略图懒加载策略** | P3 | 启动时异步加载磁盘缓存，避免卡顿 |
| 58 | **预制体预览** | P3 | 双击 `.prefab` JSON 组合 Mesh + Material 渲染 |

### 编辑器资源隔离

| # | 任务 | 优先级 | 说明 |
|:-:|:-----|:-------|:------|
| 59 | **编辑器资源与游戏 Content 分离** | P3 | 编辑器资源 → `Editor/Content/`，游戏资源 → `Content/` |

### 渲染架构演进（2026-07-28 讨论定案）

| # | 任务 | 优先级 | 说明 |
|:-:|:-----|:-------|:------|
| 22 | **MeshComponent 材质槽数组化** | P2 | ✅ **已完成（2026-08-02）**：`materialHandle` → `std::vector<MaterialHandle>`（submesh[i] → material[i]）。四端同步完成：`MeshComponent` → `MeshDesc.materials[]`（`ExportToDescription`）→ `SceneConstructor` → `OpaqueRenderItemBuilder`（按 sub-mesh 拆分渲染项）。关键修复：`Editor.cpp` 补 `SetGeometryManager`（SubMesh 展开前提）。验证：KD-03 8 槽 ↔ 8 子网格，`queueItems=10`。详见 `Docs/snapshots/MaterialSlots_Emissive_Snapshot_20260802.md` |
| 23 | **MeshEditor 属性卡** | P2 | 📋 待做（用户统一审核后推进）：注册 MeshComponent 的 ECS 属性卡编辑方法。功能：网格名/SubMesh 数量（只读）、材质槽列表（可替换）、`receivesShadow` 开关、运行时替换 `lodMeshHandle`。注意：槽位替换需双写 `m_entityDescs` 缓存（`GetMutableEntityDesc`），否则导出丢失 |
| 24 | **SubMesh 级材质替换** | P3 | ✅ **已完成（2026-08-02）**：`MeshComponent.materialSlots.size()` 与 SubMesh 数量一致，`OpaqueRenderItemBuilder` 按 slot 拆分生成独立 RenderItem。配套：**绝对索引语义定案**（BaseVertexLocation 恒 0，startVertex 仅记录）+ **顶点布局统一**（`DxMeshSkinnedVertex` 头部与静态一致，蒙皮尾部追加骨骼字段）——修复了 5 个子网格位置丢失与法线全同问题 |

> 材质槽数组化后，场景 JSON 格式变化示例：
> ```json
> "mesh": {
>     "geometry": "character",
>     "materials": ["mat_body", "mat_hair", "mat_eyes"]  // 数组，与 submesh 数量一致
> }
> ```
> 向后兼容：旧格式 `"material": "mat_body"` 等效于 `"materials": ["mat_body"]`

### 渲染管线扩展（2026-08-02 记录）

| # | 任务 | 优先级 | 说明 |
|:-:|:-----|:-------|:------|
| 60 | **G-buffer Emissive 通道** | P2 | ✅ **已完成**：G-buffer 4→5 RT（新增 `Emissive R11G11B10_FLOAT`），光照 pass `+ emissive` 加项合成（自发光不依赖光源角度）。覆盖：WindowFrameResources/Opaque+Skinned PSO/color+skinned+lighting shader/LightingRenderer 根签名/Editor 渲染循环 |
| 61 | **Emissive 数据提取修复** | P2 | ✅ **已完成**：FBX 路径 `ExtractMaterial` 补 emissive 颜色+强度两步（`AI_MATKEY_COLOR_EMISSIVE`+`AI_MATKEY_EMISSIVE_INTENSITY`）+ .mat 序列化；X 路径 `ToMaterialDesc`/`RobotMerger` 补 emissive。**X→FBX 桥发光修复闭环（2026-08-02）**：assimp 6.0.4 FBX 导出器 modern 段不写 `EmissiveColor`（只写 legacy `"Emissive"/Vector3D`，Blender 5.2 不认）→ 已通过 vcpkg overlay 补丁修复（`vcpkg-overlays/assimp/fbx_emissive_export.patch`，`assimp@6.0.4#2`），验证 `KD03_anim.fbx` 中 `EmissiveColor`/`EmissiveFactor` 各 9 次（8 材质实例+1 模板，修复前仅 1 次）。详见 `Docs/bugs/BugFix_FBXExporter_EmissiveColor.md`。遗留：`RobotMergerUtil` 的"emissive 合入 diffuse"兼容 hack 保留作可见性兜底（与新补丁不冲突，重构需单独确认） |
| 62 | **蒙皮渲染路径 SubMesh 展开** | P2 | 📋 待做：`SkinnedRenderItemBuilder` 当前只消费 `materialSlots[0]`（KD-03 走 Opaque 路径观察是临时手段）。前置：蒙皮渲染链路（骨骼缓冲 + 蒙皮 shader，P1） |
| 63 | **Blender 眼部 Emission 修正** | P3 | 📋 待做（降级）：X→FBX 桥已能写 `EmissiveColor`（#61 闭环）后，**仅旧 FBX**（合入 hack 时代导出）需要此修正；新管线从 .x 重建 FBX 不再需要。若沿用旧资产：在 Blender 给眼部材质设置 Emission（颜色+强度）重新导出，让 `.mat` 天然带非零 emissive |
| 64 | **WindowFrameResources getter 去魔法数字** | P3 | 📋 待做：getter 越界检查硬编码数量（`i>=4`→`i>=5`），与 `m_gbuffer[5]`、`GetGBufferCount()` 三处重复。改为单一事实来源（`GetGBufferCount()`/`std::size`）。详见 `SubMeshMaterialSlots.md` §5.3 |
| 65 | **TerrainRenderer GBuffer PSO 同步** | P3 | 📋 待做：当前 4 RT，接入 EditorViewport G-buffer 时需加第 5 通道（Emissive） |
| 66 | **反射探针接入** | P3 | 📋 待做：环境反射按需启用（非全局 IBL），`ComputeEnvironmentReflectionDeferred` 已有框架，需 probeIndex 来源 |

### 低优先

| # | 任务 | 优先级 | 说明 |
|:-:|:-----|:-------|:------|
| 51 | 去掉 Shaders POST_BUILD 复制 | P1 | ShaderUtils 已接管 |
| 52 | 编辑器图标/资源 | P1 | `.rc` / `.ico` |
| 53 | 场景 JSON schema 更新 | P1 | ✅ 已完成：`.ddsmesh` → `.dxmesh` 迁移完毕（Content/AssetBrowser/注册表均已用 `.dxmesh`） |
| 54 | **PhysicsScene / AudioScene / NavMeshScene 子模块** | P3 | 按需添加，后续扩展 |
| 25 | **交换链缓冲区格式检测** | P3 | 基于设备能力选择格式，当前功能检测模块未处理 |
| 26 | **GPU 资源管理器完善** | P3 | 部分模块未使用 GpuResourceManager 管理内容 |
| 27 | **输入系统 JSON 生成** | P3 | 输入配置缺少导出为 JSON 的能力 |
| 28 | **网络安全（P2P）** | P3 | 玩家身份验证、连接频率限制、加密通信：PlayerId 不能仅依赖连接句柄绑定 |
| 29 | **命令系统跨队列同步** | P3 | 当前设计不支持自动跨队列同步，需手动管理 |
| 30 | **LOD 阈值动态切换** | P3 | 当前 LOD 阈值为固定值，需要运行时动态调整能力 |
| 31 | **鼠标移出窗口限制** | P3 | 鼠标移动到窗口外时应避免影响相机旋转 |
| 32 | **实体销毁释放持久化缓存** | P2 | 实体销毁时需要同时释放持久化缓存，构建器存在多线程安全性问题 |
| 33 | **动静分批/八叉树/预计算遮挡** | P3 | 静态组件使用八叉树空间划分，预计算遮挡，动态组件单独处理 |
| 34 | **事件系统枚举处理** | P3 | 事件系统使用枚举类型需统一处理 |
| 35 | **命令列表提交前资源状态校验** | P3 | 提交命令列表前应有资源状态一致性校验 |
| 36 | **硬编码配置集中化** | P2 | 各处硬编码的配置需收敛到配置管理器 |
| 37 | **反射探针优先级可配置** | P3 | 反射探针的更新优先级从硬编码改为可配置 |
| 38 | **单位系统统一** | P3 | 当前基于约定 1.0f = 1 米，后续需正式定义并统一 |
| 39 | **镜面效果优化** | P3 | 展示的镜面效果需后续优化处理 |
| 40 | **描述符堆场景切换** | P2 | 描述符堆需考虑场景切换时的分配与释放策略 |
| 41 | **描述符槽环形缓冲区** | P3 | 描述符槽分配器增加 Ring Buffer 模式替代 FreeList |
| 42 | **可见集射线检测对齐** | P2 | VisibleRaycaster 与 CullingSystem 的可见集需对齐，当前两者脱节导致拾取精度受影响 |
| 43 | **阴影资源格式统一** | P3 | 方向光阴影 `DirShadowResources` 与点光源的 `DepthStencilHandle` 格式不一致，需统一 |
| 44 | **材质系统纹理索引化** | P3 | 网格组件持有的 `textureHandle` 逐步剥离为 `gTextureMaps[]` 无界数组下标，资源生命周期由纹理池管理 |
| 45 | **阴影剔除队列独立** | P2 | 阴影 Pass 应从光源视锥体做剔除，生成独立 `m_shadowCasterQueue`，避免相机看向空区域时阴影停滞 |
| 46 | **阴影矩阵增量更新** | P3 | `ComputeDirShadowMatrix` 可优化为仅在相机视锥体变化时重建 |
| 47 | **key/hash 编辑器应用** | P3 | key 与 hash 在编辑器下的应用场景定义 |

### 基础设施改进（历史遗留）

| # | 任务 | 优先级 | 说明 |
|:-:|:-----|:-------|:------|
| 48 | **配置管理器 JSON 合并** | P3 | 多配置文件未在内存中合并 |
| 49 | **系统模块资源释放** | P3 | 确保所有系统模块可正常释放资源 |
| 50 | **事件系统挂起唤醒机制** | P3 | 协程 Task 支持 + 等待队列管理 + 事件触发唤醒 + TaskFlow 集成 |

---

## 三、核心文件结构

```
Engine/
  ├─ Scene/
  │   ├── SceneManager.h/.cpp         ← 基类，内部持有 Registry + RenderScene
  │   ├── SceneConstructor.h/.cpp      ← 场景构造器
  │   └── RenderScene.h/.cpp           ← 渲染上下文容器
  ├─ Boot/
  │   ├── Bootstrap.h/.cpp            ← 装配层，SceneManager 值成员
  │   └── GameContext.h               ← 轻量 DI 容器
  ├─ Renderer/RenderItemBuilder/
  │   └── OpaqueRenderItemBuilder.h/.cpp ← 支持实体过滤器
  └─ Scheduler/
      └── FrameDriver.h/.cpp

Editor/EditorLib/
  ├─ Core/
  │   ├── Editor.h/.cpp               ← Editor 主入口
  │   └── EditorLayout.h/.cpp         ← 布局管理器（TabBar 回调注册）
  ├─ ECS/
  │   └── SceneTagComponent.h         ← 编辑器端专用标记组件
  ├─ Panels/
  │   ├── AssetBrowser.h/.cpp         ← 资产管理器（含 SceneConstructor 生命周期）
  │   └── OutlinerPanel.h/.cpp        ← 场景大纲（使用 GetActiveEntities）
  └─ Scene/
      ├── EditorSceneManager.h/.cpp   ← 场景管理器（多 Tab + 标记组件 + 缓存）
      └── EditorStateFile.h/.cpp      ← 编辑器状态持久化

Game/Game/
  ├─ Game.h/.cpp                        ← 主入口 + 主循环
  ├─ Input/
  │   ├── GameInputActions.h            ← 游戏动作定义
  │   └── GameInputHandler.h/.cpp       ← 游戏输入处理（相机、拾取、拖拽）
  ├─ RenderPipeline/
  │   └── GameRenderPipeline.h/.cpp     ← 构建器/渲染器/队列 + 16 个系统注册
  ├─ Resources/
  │   └── GameResources.h/.cpp          ← GPU 资源初始化（白纹理、预触）
  └─ Scene/
      ├── GameWorld.h/.cpp              ← 主循环 + 子模块编排
      ├── GameSceneManager.h/.cpp       ← 场景生命周期
      └── (旧文件已删除)
```

---

## 三、水渲染管线重构（2026-08-04 定案）

> 关联：`Docs/architecture/rendering/RenderPipelineSpecification.md`（渲染管线规范）、`Docs/snapshots/RenderPipeline_Snapshot_20260804.md`（当前状态快照）
>
> 目标：水渲染从"旧标记模式（TransparentTag）+ ECS view 遍历"迁移到"标准材质槽 + 桶模式"，
> 同时修复 Editor 水误录 Opaque 阶段等已知缺陷。
>
> **当前范围：仅编辑器端。Game 端暂不处理，待编辑器端验证通过后再同步。**
> 分阶段执行，每个阶段完成后再进入下一阶段。

### 阶段 0：清理编辑器旧水渲染代码

| 步骤 | 文件 | 改动 |
|:--|:--|:--|
| 0a | `Editor/EditorLib/Core/Editor.cpp` | 删除 `EditorOpaqueRenderSystem` 内联的水渲染（1155-1190 行） |
| 0b | `Editor/EditorLib/Core/Editor.cpp` | 删除 `m_waterRenderer` 初始化（199-206 行） |
| 0c | `Editor/EditorLib/Core/Editor.cpp` | 删除 `WaterManager::CollectFromECS` 调用（520-521 行） |
| 0d | 编译验证：编辑器无水渲染，不崩溃 | 人工编译 |

### 阶段 1：MPD .scene 二进制 → 水实体构建（编辑器验证）

> 确保 `MapSceneConverter` 输出的 `.scene` 二进制（`waterBlocks` 数组）被 `SceneLoader` 解析后，
> `SceneConstructor` 能构建出标准的水实体组件序列。此阶段只验证加载路径，不涉及渲染。

| 步骤 | 文件 | 改动 |
|:--|:--|:--|
| 1a | `Engine/Asset/IO/Loader/SceneLoader.cpp` | 验证二进制 .scene 的 `waterBlocks` 解析（`LoadSceneBinary` 已有读 `waterBlocks`） |
| 1b | `Engine/Asset/IO/Loader/SceneLoader.cpp` | 验证 JSON 场景的 `waterBlocks` 解析（`ParseSceneJSON` 读 `waterBlocks` 数组） |
| 1c | `Engine/Asset/IO/Loader/SceneLoader.cpp` | 新增 `waterBlocks` 的 JSON 序列化（`to_json`）与 `ParseWaterBlock` 函数，支持 JSON 回写 |
| 1d | `Engine/Scene/SceneConstructor.cpp` | `OnSceneConstructReady` 中，将 `waterBlocks[]` 依次转换为标准实体：<br>• TransformComponent（world 的 pos/scale → 世界矩阵 + cullDistance）<br>• MeshComponent（lodMeshHandle ← 程序化网格 GeometryProceduralTask 注册）<br>• RenderSlotComponent（materials[0] = Water 材质，subMeshRanges = [{0, indexCount}]，shaderType = Water）<br>• WaterComponent（波浪参数） |
| 1e | 验证：编辑器加载 .scene 后水实体组件序列完整 | 人工编译 + 调试视图检查 |

### 阶段 2：程序化网格 GeometryProceduralTask（编辑器验证）

> 水面的 grid 程序化生成需要走 `GeometryProceduralTask`，注册到 `GeometryResourceManager`，
> 返回 `GeometryHandle`，供 `MeshComponent` 引用。编辑器端先验证。

| 步骤 | 文件 | 改动 |
|:--|:--|:--|
| 2a | 新建 `Engine/Resource/Procedural/GeometryProceduralTask.h/.cpp` | 实现 `type="grid"` 的程序化网格生成（顶点/索引/SubMeshInfo/localBounds） |
| 2b | `Engine/Resource/Procedural/GeometryProceduralTask.h/.cpp` | 注册到 `GeometryResourceManager`，返回 `GeometryHandle` |
| 2c | `Engine/Scene/SceneConstructor.cpp` | `ConstructEntity` 的 mesh 分支：`eDesc.mesh.procedural` 存在时触发 `GeometryProceduralTask`，替代 `geoMap.find(m.geometry)` |
| 2d | 验证：编辑器加载水实体 JSON 后网格正确注册 + 组件完整 | 人工编译 + 调试视图检查 |

### 阶段 3：水渲染器桶模式迁移 + 编辑器重新实现

> `WaterRenderItemBuilder` 迁移到桶消费模式，编辑器端重新实现 `EditorWaterRenderSystem`。

| 步骤 | 文件 | 改动 |
|:--|:--|:--|
| 3a | 新建 `Engine/Renderer/RenderItemBuilder/WaterRenderItemBuilder.cpp`（桶模式） | 实现 `ForEachBucket("water")` 消费，从 `WaterComponent` 读波浪参数 |
| 3b | `Editor/EditorLib/Core/Editor.cpp` | 注册 `EditorWaterRenderSystem`（RenderPhase::Transparent），从 Editor 内联拆出 |
| 3c | 验证：编辑器水正确渲染在 Transparent 阶段 | 人工编译 + 运行验证 |

### 后期（Game 端同步，待编辑器端验证通过后）

| # | 任务 | 说明 |
|:--|:-----|:------|
| G1 | Game 端删除旧 `RegisterWaterRenderSystem` | 同步阶段 0 |
| G2 | Game 端注册桶模式 `WaterRenderSystem`（RenderPhase::Transparent） | 同步阶段 3 |
| G3 | 实现 `TransparentRenderSystem` 消费 `m_transparentQueue` | 独立新增 |

### 已知缺陷复盘（2026-08-05 更新）

| # | 缺陷 | 解决阶段 | 状态 |
|:--|:--|:--:|:--:|
| #1 | Editor 水渲染误录 Opaque 阶段 | 阶段 3b | ✅ 已修复（删除 EditorOpaqueRenderSystem 内联水渲染块，水只走 Transparent 阶段 EditorWaterRenderSystem） |
| #2 | Game 透明队列无消费系统 | 后期 G3 | ⏸ 暂缓 |
| #3 | WaterRenderItemBuilder 未迁移桶模式 | 阶段 3a | ✅ 已修复（`ForEachBucket("water")` 桶消费） |
| #4 | ProceduralGeometryDesc 未接入 SceneConstructor | 阶段 2c | ✅ 已修复（AssetManager 虚拟资产 `procedural://` URI + `LoadScene` 收集 + `ConstructEntity` 标准组装） |
| #5 | Game/Editor 渲染阶段不一致 | 阶段 3b + 后期 G2 | ❌ 未开始 |

### 水渲染管线重构（2026-08-05 更新）

当前方案（GeometryProceduralTask + 异步组件组装）已被**未来方向替代**：

| 维度 | 当前方案（已放弃） | 未来方向（AssetManager 虚拟资产） |
|:--|:--|:--|
| 程序化网格入口 | `GeometryProceduralTask::Create` 直接调用 | `AssetManager::Load("procedural://grid/...")` URI 引用 |
| 组件组装 | `OnWaterTaskCallback` 手动添加 MeshComponent + RenderSlotComponent | `SceneConstructor::ConstructEntity` 统一走 mesh 分支 |
| 桶分发 | 依赖八叉树粗筛集（`m_octreeCoarse`），异步创建的水实体不在八叉树中 | 标准实体创建流程，`CreateEntity` 自动更新八叉树 |
| 复杂度 | 高：堆分配回调上下文 + MSVC lambda ICE 规避 + 手动组件组装 | 低：URI 引用 + 标准加载管线 + 自动组件组装 |

### 已知缺陷（2026-08-05 更新）

| # | 缺陷 | 说明 | 状态 |
|:--|:--|:--|:--:|
| W1 | AssetManager 虚拟资产管线已实现、水纹理未输入 | ✅ 已解决：水材质改为 `Water/Sim` + sea 纹理（根目录 City.scene.json 4 个 WaterBlock），`shaderType=Water` 路由正确，RenderDoc 确认水面带纹理 + 流动性 | ✅ |
| W2 | Game 透明队列无消费系统 | 暂不处理 | ⏸ 暂缓 |
| W3 | 已废弃（AssetManager 方案已解决八叉树问题） | ✅ | ✅ |
| W4 | Transparent 阶段的 RT 状态紧挨天空盒 | 阶段顺序正确，barrier 已修复 | ✅ 已验证 |
| W5 | 已废弃（AssetManager 方案已替代旧方案） | ✅ | ✅ |

### 新增待办（2026-08-05 会话登记）

| # | 任务 | 说明 | 优先级 |
|:--|:--|:--|:--:|
| N1 | **转换工具水块缝隙修复** | `MapSceneConverter.cpp:664-666` 四象限中心公式 Z 方向错位 1 格（间距 390 ≠ 网格 420，缝隙 30 单位）。⚠️ 2026-08-05 复核：四象限公式数学上无缝（中心公式与半宽对齐，奇数格也验证），city 缝隙是旧数据（早期 flood fill 时代输出），非当前转换器 bug——待重转验证 | P1 |
| N2 | **水位基准高度从 MPD 获取** | ✅ 转换器已实现：`MapSceneConverter` 从 Sea 实例取**最低 Y** 填 `waterY`（JSON `world[1]` + `wbBin.posY`，替代硬编码 0）。实测输出 posY=-0.5（对齐 MPD `#WaterY`）；编辑器手动值 -1.0 为临时，重转后自动正确 | ✅ |
| N3 | **水 UV 世界坐标化** | ✅ 已完成：`water.hlsl` VS `vout.TexCoord = worldPos.xz * gUVTiling`（跨块连续）；`WaterConstants` 加 `UVTiling` 字段（补 Pad4 对齐 64B）；`WaterManager::SetUVTiling(0.01)` | ✅ |
| N4 | **Game 端接入岸线渐隐** | `GameRenderPipeline.cpp` WaterRenderSystem 未绑定 depthSRV（`BeginFrame` 不传，`SetFadeRange` 默认 0 降级）。接入：传 `GetDepthSRV()` + 补 `DEPTH_WRITE → DEPTH_READ → DEPTH_WRITE` 对称屏障（参照 Editor 端，见 `WaterRenderingTechniques.md §7.1`） | P2 |
| N5 | **CrazyBump 水贴图生成** | 从无缝源图生成 Normal/Specular/Height/AO 贴图（清单见 `WaterRenderingTechniques.md §8.4`），接入材质槽 | P3 |
| N6 | **水法线贴图 + TBN** | `water.hlsl` PS 采样法线贴图需 VS 输出完整 TBN（含 Bitangent），双层法线反向 panner（§8.2B） | P3 |
| N7 | **P3 区域差异化** | WaterInfo 纹理 / Water Mask（R/G/B 频带），大水面内不同区块不同浪（Unity WaterMask 同款） | P3 |
| N8 | **P4 焦散光斑** | Caustics 纹理斜向投影（Unity Caustics 同款） | P4 |
| N9 | **地面合并程序化化** | ✅ 转换器已实现：`MapSceneConverter` 新增 `IsMergeableGround`（mapChip 单材质纯平面）+ 贪心最大矩形分解（直方图法）→ `procedural://grid/W/H/segX/segZ` 四边形块。已修复：覆盖格展开（世界矩阵变换角点，镜像/旋转不错位丢格）、CreateGrid UV 每段平铺（`TexC=j/i` 整数）、Water 材质补 `textures.baseColor="sea"`、waterBlocks 数组 → 标准水实体。**自适应分段**：解析原始 .x UV 平铺周期（`groundUVTiling[stem]`，如 mapChip06 UV∈[0,10] → 每 30 单位格平铺 10 次 → 每 3 单位一个纹理周期）→ 合并块分段 = 格数 × 平铺周期，纹素密度对齐原始 .x（256²/3 单位 ≈ 85 纹素/单位），处理其他地图自动适配；无 UV 数据回退 ×3（10 单位/quad）。**已验证**：CLI 重转 810×90 → `270×30`（×10 自适应），用户确认纹理密度正常、无拉伸 | ✅ 已完成 |
| N10 | **体素型光照（VoxelShading）** | 根因：582 个 mapChip 全部 X 镜像但 precomputed 烘焙丢失 scale 负号（`world[0]/worldInvTranspose[0]=1` 应为 -1）→ 法线方向错 → 每块明暗不一致。修复：① 合并地面（N9）连带解决拼接块；② 烘焙端纳入负号；③ 风格化开关 `FlatShading`（`ddx/ddy` 面法线，体素游戏用）。详见 `Docs/effects/VoxelShadingLook.md` | P2 |

### 2026-08-05 会话水渲染成果汇总

| 成果 | 说明 |
|:--|:--|
| 图元继承重构 | `GeometryBase` 统一（VB/IB + subMeshes + topology），TriangleMesh/GridGeometry/PatchMesh 继承；`GetGeometryBase()` 统一访问；Opaque/Shadow/Skinned/Sky/Reflection/Terrain/Water 渲染器全部改用 |
| 程序化网格接入 | `SceneConstructor::LoadScene` 收集 `procedural://` URI → AssetManager 虚拟资产 → `ConstructEntity` 标准组装；GridGeometry 显式填充 subMeshes |
| 桶模式 + Editor 系统 | `WaterRenderItemBuilder` 桶消费（`ForEachBucket("water")`）；`EditorWaterRenderSystem`（Transparent 阶段）；删除 Editor 内联旧水渲染 |
| §10.5 数据上传铁律 | Builder 不分配 CB / FrameSync 统一上传；删除 `WaterObjCB_Persistent` 持久 CB；`ObjectConstants`（World/WorldInvTranspose/MaterialIndex）FrameSync 每帧上传 |
| P2 岸线深度渐隐 | 根签名 slot 7（t11 场景深度 SRV）；water.hlsl `LinearDepthFromNDC` 线性化水深 → `shoreFade`；Editor 端 `GetDepthSRV()` + `SetFadeRange(2.0)` 生效 |
| 水位调整 | 4 个 WaterBlock y: 0 → -0.5 → **-1.0**（临时手动值，待 N2 从 MPD 获取） |
| 波形世界坐标 | water.hlsl VS 波形 `vin.PosL` → `worldPos`（多块水面边界运动连续，消除运行时缝隙） |
| 文档 | `WaterRenderingTechniques.md`：§8.4 CrazyBump 工具链、§8.3 现状更新、§7.1 P2 落地记录 |

### 2026-08-05 地面合并转换验证记录（原始版 vs 处理版）

**试运行**：`AssetTool.exe mpd2scene "D:/APP/.../map/City" → out_test/`（pieces 40, instances 7348, materials 37, textures 27）

**差异对比**：

| 维度 | 原始版（Content/City，手动改） | 处理版（out_test，重转） | 说明 |
|:--|:--|:--|:--|
| 实体数 | 7320 | 7020（-300） | mapChip06 合并 324→33（-291）；waterBlocks 数组化（-4 实体）等 |
| 水面 | 4 个 `WaterBlock` 实体（`procedural://grid/420/420/32/32`，y=-1.0） | **waterBlocks 数组**（4 个，posY=-0.5），无实体 | 引擎加载时 `EditorSceneManager.cpp:1036` 自动转实体 ✅ 非断链 |
| 水位基准 | -1.0（手动临时值） | **-0.5**（Sea 实例最低值，对齐 MPD `#WaterY`） | 处理版正确 |
| 水块尺寸 | 4 × 420×420 | 2 × 420×420 + 2 × 420×360（tiling 14×14/14×12） | 2×2 划分 Z 方向不对称（海区非矩形） |
| mapChip06 | 324 独立实体 | 33 程序化块（810×90/90×690/90×630/660×30 等）+ 253 多材质残余（mapChip03/04/05） | 合并成功 ✅ |

**待确认异常**（需用户提供具体现象或加载 out_test 场景观察）：
1. 水位 -0.5 vs -1.0（水面若浮出地面与此相关）
2. 水块 420×360 不对称（Z 14+12 格）
3. 合并块渲染（程序化块法线/纹理/UV 表现）

**2026-08-05 第二轮排查（合并块纹理拉伸，用户反馈"一整块纹理图案，无平铺"）**：

| 排查点 | 结论 |
|:--|:--|
| CreateGrid UV（`GeometryGenerator.cpp:577-578`） | ✅ 已改为每段平铺 `TexC = j/i` 整数（旧 `j*du` 已注释） |
| 编译产物时间戳 | ✅ 源码/obj/lib/Editor 全部一致（18:58） |
| GeometryProceduralTask UV 传递 | ✅ cpuWork 直接 memcpy CreateGrid 输出，无覆盖 |
| **运行时 UV 日志**（DebugView `[ProcGeo][Diag]`） | ✅ **顶点 UV 确认为整数**（810×90@27/3 → `UV[0]=(0,0) UV[1]=(1,0) UV[2]=(2,0)`）——CreateGrid 修改**已生效** |
| color.hlsl VS/PS | ✅ VS `vout.TexCoord = vin.TexCoord` 透传；PS 直接采样，无缩放/归一化 |
| 采样器 s4（gSamplerAnisotropicWrap） | ✅ `PSOFactory.cpp:353-354` = `D3D12_FILTER_ANISOTROPIC + TEXTURE_ADDRESS_MODE_WRAP` |

**矛盾点**：顶点 UV 整数 + 透传 + WRAP 采样，数据链路全部正确，但**视觉仍是一整块纹理图案（无平铺）**。
**待查方向**：① 渲染项 VB 布局/stride 与 GridGeometry 实际布局是否一致；② 纹理资源本身（mat_b989dce8703d 引用的 DDS）；③ RenderDoc 旧捕获误导。

**2026-08-05 第三轮（根因确定 + 自适应分段落地 ✅）**：

| 环节 | 结论 |
|:--|:--|
| **原始 .x UV 解析**（mapChip06.dxmesh 保留） | UV 范围 **0~10**（非 0~1）——30 单位格内平铺 10 次 → **每 3 单位一个纹理周期** |
| 纹素密度对比 | 原始 256²/3 单位 ≈ **85 纹素/单位**；合并块原 ×3（10 单位/quad）= 25.6 → **差 3.3 倍 = 视觉"拉伸/太小"根因** |
| **自适应分段**（`MapSceneConverter`） | 拆解 piece .x 时统计 `uvMaxU/uvMaxV` → `groundUVTiling[stem]` → 合并块分段 = 格数 × 平铺周期；无 UV 数据回退 ×3 |
| 验证 | CLI 重转 810×90 → `procedural://grid/810/90/270/30`（×10，每 3 单位一个 quad）——**用户确认纹理密度正常、无拉伸** |

**结论**：合并块纹理"拉伸"根因是**分段不足导致纹素密度低**（非 UV 生成/透传/采样问题）——CreateGrid UV 整数平铺本身正确，但每 10 单位一个 256² 纹理周期密度不够；自适应分段（从 .x UV 平铺周期推导）对齐原始密度后解决。处理其他地图自动适配。

**2026-08-05 第四轮（用户确认：水渲染正常 + 自适应分段多地图验证 ✅）**：

| 验证点 | 结论 |
|:--|:--|
| **水渲染** | ✅ 水块纹理输入正常（08-05 快照 P1 已闭环）。根因确认为 `MaterialData::name` 被设为材质 key 而非 shader 字符串，`ParseShaderType` 返回 Unknown 影响纹理绑定；修复点为 `MaterialLoadTask.h:78`（name = .mat 的 shader 字段）+ `SceneConstructor.cpp:664`（`ParseShaderType(md->name)` 路由） |
| **自适应分段** | ✅ 资产工具按 .x UV 平铺周期（`uvMaxU/uvMaxV` → `groundUVTiling[stem]`）自适应分段合并区块，用户确认其他地图上纹理密度正常、无拉伸 |

**闭环确认**：合并块纹理"拉伸"与水面渲染两个 08-05 遗留问题均已解决。

---

## 四、GPU Driven 分层剔除（2026-08-06 定案 + 阶段 0 落地）

> 关联：`Docs/architecture/rendering/GPU-Drive.md`（分层蓝图）、`Docs/snapshots/GPUDriven_Snapshot_20260806.md`（快照）、
> `08_MapScenePipeline.md` §8.6/§8.7（区块化聚合 + 块配置化）
>
> 分层：**L1 CPU 块级粗筛（✅ 已有）→ L2 GPU 实例级剔除（📋 待建）→ L3 遮挡（🔭 远期）**。
> 关键认知：ECS 实体数减少（15489→5 块），渲染实例数据量不减；**精细剔除从 Builder（CPU）下放 GPU**。
> 集群（BlockComponent）= **纯剔除豁免器**（对抗远近裁剪面），forceVisible 只让块实体进候选集，块内实例 L2 剔除照常。

| 阶段 | 任务 | 优先级 | 状态 |
|:--|:--|:--|:--:|
| 0a | `BlockConfigDesc` 结构体 + to_json/from_json（SceneDescription.h） | P1 | ✅ 2026-08-06 |
| 0b | `ParseBlockConfig`（SceneLoader.cpp LoadFromJSON + SaveToJSON） | P1 | ✅ 2026-08-06 |
| 0c | `SceneConstructor` 加载推导 cellSize = clamp(mapExtent/blocksPerAxis) + Phase C 消费 blockCellSize | P1 | ✅ 2026-08-06 |
| 0d | `ExportToDescription` 保存固化 blockConfig（SceneSnapshot 缓存 + 写回） | P1 | ✅ 2026-08-06 |
| 0e | `Schemas/scene.schema.json` 加 blockConfig 定义 | P1 | ✅ 2026-08-06（JSON 校验通过） |
| 1a | 块实体入空间哈希 + SceneTag（OctreeSystem::AddEntity 存块而非实例实体，clusterBounds 修复） | P1 | ✅ 2026-08-06 |
| 1b | RenderSlotCache 桶存块条目，Builder 消费块跳过逐实例视锥 | P1 | ✅ 2026-08-06 |
| 2a | 集群豁免验证（forceVisible/clusterBounds 与 L2 正交性） | P1 | ✅/验证 |
| 3 | L2a 实例矩阵 + 包围球半径 → StructuredBuffer（`InstanceCullingBuffer`） | P1 | ✅ 2026-08-06 |
| 4 | L2b Compute 视锥剔除 + AppendBuffer + IndirectArgs（`InstanceCulling.cs.hlsl`） | P1 | ✅ 2026-08-06 |
| 5 | L2c readback 验证链路（`ReadbackVisibleCount` 延迟 1 帧）；**完整间接绘制（DrawIndexedInstancedIndirect）⏸️ 延后至 GS 阶段** | P1 | ✅ 2026-08-07 / ⏸️ GS 阶段 |
| 6 | L3 HZB 深度遮挡 / 保守化 PVS（只剔小物体，排除地块） | P3 | 🔭 远期 |

**下一步**：人工编译验证（项目规则 AI 不编译）→ 阶段 5 完整间接绘制（GS 阶段）→ 阶段 6 L3。

### 4.1 运行期修复（GBV 严格模式暴露，2026-08-07 全部落地）

| # | 问题 | 修复 |
|:--|:--|:--|
| #935 | lighting.hlsl 根参数未绑定（ssao/envMap/shadow） | "着色器 if"方案：cbLights `gHasSsao/gHasEnvMap/gHasShadow` 标志，shader 无资源跳过采样；`SetHasSsao/SetHasEnvMap/SetHasShadow` 值变化置脏重传 |
| #942 | COPY 列表资源状态 | CORE 上传模式统一：COPY_DEST → COMMON 创建 + DIRECT 转出（MeshLoadTask/GeometryProceduralTask/SceneConstructor/PreviewPBRRenderer/InstanceCullingBuffer） |
| #646 | **LightingRenderer.cpp:198 崩溃（envMapSrv 无效描述符，句柄 0x8000...）** | **三层根治（✅ 08-07）**：① 引擎 CORE `BlankTextureProvider`（White2D/BlackCube，EditorViewport 堆域）注入 AO/Skybox/Water fallback；② **`LightManager.cpp` 38 处描述符堆调用补 `m_heapTag`**（shadowDataSRV/shadowMapSRV 原走默认 Default 堆，与 Editor 光照 pass 绑定的 EditorViewport 堆不匹配 → GBV #935/#646；规则 17 违规）；③ **`AO::CpuSrvToGpu`/`BuildRandomVectorTexture` 与 `SsaoRenderer::ComputeAO/BlurAO` 同模式缺 `m_heapTag`**——CpuSrvToGpu 用 Default 堆基址计算 EditorViewport 堆 CPU 句柄偏移 → 跨堆垃圾 GPU 句柄（恰为 0x8000... 特征）；SSAO pass 绑定 Default 堆却使用 EditorViewport 堆的 depth/normal/AO SRV。修复：AO.cpp 补 5 处、SsaoRenderer 新增 `SetHeapTag`+成员补 2 处、AO::Initialize 注入。`#646` 为执行期错误，断点停靠行≠出错绑定点。待人工编译验证 |

### 4.2 InstanceCulling RingBuffer 内存增长 + GPU TDR（2026-08-08 调查，⚠️ 问题仍存）

> 现象：`LightManager::UpdateAndUpload:207` 断言 `mapped != nullptr` 崩溃（ucrtbased.dll 栈），**不控制相机也复现**
> 证据链：log.txt（fmt error ×2 → `std::bad_alloc` → **GPU TDR DEVICE_HUNG** → 设备移除 → Map 全失败 → 断言）+ VS2022 内存曲线（**逐步增长至 ~992MB/1GB 后跌落崩溃**）
> 结论：LightManager 是**设备移除后第一个碰 GPU 的调用点（最后一环）**，非光源问题；真正的根因是每帧内存累积 + RingBuffer 扩容
> 快照：`Docs/snapshots/InstanceCulling_MemoryGrowth_TDR_Snapshot_20260808.md`
> 关联：`Docs/architecture/core/Frame.md`（扩容权威设计：1.5x 增长 + 硬上限 + 多段延迟回收）

| # | 问题 | 修复/状态 |
|:--|:--|:--|
| M1 | `ApplyTabState:1299` fmt 格式串 5 占位符/4 参数（缺 lights）→ `fmt::v12::format_error` | ✅ 已修：补 `snap.lightDescs.size()` |
| M2 | `GpuHandlePool::FreeSlot` 用 `Validate()`（拒绝 PendingRelease）→ Update 释放路径槽位永不回收、dataPtr 悬垂 → 句柄池泄漏 | ✅ 已修：改为 generation 匹配 + 非 Empty 校验，允许回收 PendingRelease |
| M3 | `AllocateWithRetry` 扩容 ×2 无上限（16→1GB 指数膨胀）+ `Initialize` 内部销毁 GPU 在用旧资源（悬垂 → TDR） | ✅ **方案 B 已落地**：FrameResourceManager 多段缓冲池（1.5x 增长 + 256MB 上限 + 旧段 fence 延迟回收）；`InstanceCulling` 条目 16MB→64MB |
| M4 | **方案 B 落地后问题仍存（用户确认）** | 📋 待查：① `CommandManager::EndFrame` 用 `GetCurrentSequence()`（不递增）signal vs `BeginFrame` 用 `GetNextFence()`（fetch_add 递增）——若全局序号未正确推进，`Reclaim` 永不回收 → 每帧分配累积；② `[InstanceCulling][Verify] visible=0 / total=6853 (0.0%)` 剔除链路异常；③ VS2022 曲线复查（扩容是否仍触发、Output 是否出现 `[WARN] new segment`）；④ 其他每帧累积物排查 |

**下一步**：人工编译验证（项目规则 AI 不编译）→ 查 `CommandManager::EndFrame` signal 序号递增 → VS2022 曲线复查。

---

## 五、相关文档

- `Docs/architecture/scene/SceneManager.md` — 完整架构设计（含 §10 多 Tab 架构 + 事件流）
- `Docs/architecture/editor/ComponentEditorSystem.md` — 组件驱动属性卡与 ImGuizmo 集成设计
- `Docs/architecture/core/InputSystem.md` — 输入系统架构（三层数据模型、TriggerBehavior 语义、数据流图）
- `Docs/architecture/core/EngineOverview.md` — 引擎架构总览（含 Game 端最新文件结构）
- `Engine/Scene/SceneManager.h` — 核心接口定义