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

## 四、相关文档

- `Docs/architecture/scene/SceneManager.md` — 完整架构设计（含 §10 多 Tab 架构 + 事件流）
- `Docs/architecture/editor/ComponentEditorSystem.md` — 组件驱动属性卡与 ImGuizmo 集成设计
- `Docs/architecture/core/InputSystem.md` — 输入系统架构（三层数据模型、TriggerBehavior 语义、数据流图）
- `Docs/architecture/core/EngineOverview.md` — 引擎架构总览（含 Game 端最新文件结构）
- `Engine/Scene/SceneManager.h` — 核心接口定义