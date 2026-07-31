# 全局待办清单

> 日期：2026-07-27
> 本次会话：`CameraComponent` ECS 结构体创建 + SceneConstructor 接入 + CameraEditor 注册 + ExportToDescription 导出。实体关系模型设计定案（扁平 JSON + ID 引用，引擎 CORE 只存不处理）。关系设计文档参见 `Docs/architecture/RelationshipModel.md`。
>
> 详细快照见 `Docs/snapshots/WorldRefactor_Snapshot_20260723.md`。
>
> **重要设计决策**：
> 1. 编辑器端采用标记组件方案（`SceneTagComponent`），所有场景实体共存于同一 ECS Registry，通过 `sceneId` 区分。切换 Tab 时不动 Registry，只更新活跃索引，Builder 按 `sceneId` 过滤。详见 `Docs/architecture/SceneManager.md §10`。
> 2. 属性卡采用 ECS 组件驱动注册制：`ComponentEditorRegistry::Register<T>(drawFn)`，每个组件类型独立注册编辑方法，控制逻辑分散到各组件。ImGuizmo 集成用于 3D 变换操作。详见 `Docs/architecture/ComponentEditorSystem.md`。
> 3. 输入系统改为声明式推送模式：`InputSystem::BindCallback()` 注册回调，`InputSystem::Update()` 末尾自动触发。System 在注册时通过 `SystemInfo::inputDeclarations` 声明输入需求。详见 `Docs/architecture/InputSystem.md`。
> 4. **SceneManager 不是所有 ECS 实体的源头，ECS Registry 才是。** SceneManager 只做场景实体的 CRUD（增删查不改），组件值修改由各 System 自行负责。选中实体独立为 SelectionService，不依赖 SceneManager。详见 `Docs/architecture/SceneManager.md §11`、`Docs/architecture/ViewportToolbar.md §十二`。

---

## 二、待办

### 可恢复的功能

| # | 任务 | 优先级 | 说明 |
|:-:|:-----|:-------|:------|
| 1 | 公告牌手动放置（JSON 驱动） | P2 | 已有 `BillboardDesc`/`BillboardComponent`/Builder，只需恢复注册 + 接入 |
| 2 | 公告牌程序化体积生成 | P3 | `BillboardVolumeDesc` + CPU 生成，见 `BillboardSystemArchitecture.md` |
| 3 | 蒙皮角色 JSON 加载 | P3 | 需 AssetTool 将 `.m3d` 导出为 `.dxmesh`（含 `DxMeshFlag_Skinned`） |

### 基础设施改进

| # | 任务 | 优先级 | 说明 |
|:-:|:-----|:-------|:------|
| 4 | `DxMeshLoader` 完整实现 | P1 | 当前 `MeshLoadTask` 已读 `.dxmesh`，但需完善骨骼数据路径 |
| 5 | **组件驱动属性卡** | P1 | 基于 `ComponentEditorRegistry` 注册制的属性卡系统，替代当前硬编码的 Properties 面板。<br>• ✅ `ComponentEditorRegistry` 注册器（模板 Register<T> + 回调存储）<br>• ✅ `EditorLayout::DrawProperties()` 已重构为遍历注册表 + 按组件类型折叠 + 添加/移除按钮<br>• ✅ `RegisterTransformEditor()` 已注册（Position/Rotation/Scale 数值输入 + 四元数↔欧拉角转换）<br>• ✅ `RegisterLightEditor()` 已注册（类型/颜色/强度/范围/阴影参数）<br>• ✅ "添加组件" 弹出菜单（只显示实体上不存在的组件）<br>• ✅ "移除组件" 按钮（Transform 等核心组件不可移除）<br>• ✅ 编辑器 UI 标签已接入 `EditorStrings::Get()`（TransformEditor/LightEditor 均已迁移）<br>• ✅ `CameraComponent` 已创建（ECS 结构体 + SceneConstructor 创建 + CameraEditor 注册 + ExportToDescription 导出）<br>• ⚠️ `CameraComponent` 属于 **Scene Entity Data** 层，非引擎 CORE。后续设计讨论：投影类型（Perspective/Orthographic）、PiP 预览、视锥 Gizmo |
| 7 | 多堆域 SRV 隔离（SkyboxManager → LightManager 等） | P2 | SkyboxManager 已支持 `HeapTag` 参数，SRV 创建在指定堆域。LightManager、ReflectionProbeManager 等需同样扩展，使其在多堆（Editor）模式下不占用 Default 堆域空间 |
| 8 | AssetManager 注册表模式 | P2 | 见 `AssetLoaderImprovement.md`，用文件后缀分发替代 `switch(type)` |
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

### ECS 统一数据源 + Manager 打包器设计（2026-07-27 补充，2026-07-28 更新）

详见 `Docs/architecture/EngineOverview.md §9`：

- ECS Registry 是运行时唯一数据源，Manager **不持有私有数据**，只做 ECS → GPU buffer 的聚合打包
- 需要剔除/拾取/属性卡编辑的数据（灯、相机、水面）必须在 ECS 中有对应的 Component
- Manager 存在的唯一理由：多个实体的数据需要聚合为连续 GPU buffer，或管理 GPU 资源（shadow map、纹理数组）
- 过渡路径：CameraComponent → LightManager → WaterManager 逐步迁移
- 场景 JSON 统一由 SceneConstructor 写入 ECS 组件，Manager 不再接收直接注册调用

**Manager 收集模式实施状态**（CameraManager ✅ 已有、LightManager ✅ 已实施、WaterManager ✅ 已实施），详见 `Docs/architecture/EngineOverview.md §9`。

**SceneConstructor 变更**：
- `LightComponent` 创建从 TODO 占位变为完整实现，支持 directional/point/spot 三种类型
- `WaterComponent` 波浪参数直接写入 ECS（amplitude/frequency/speed/direction），不再调用 `WaterManager::RegisterWaveParams`

#### SceneEnvironment 语义边界（2026-07-28 补充）

并非所有场景数据都能放入 ECS 实体。详见 `Docs/architecture/EngineOverview.md §9.6`：

- **T 恤线**：有 `TransformComponent`（位置/变换）的数据 → ECS 实体 `entities[]`；无实体身份的全局参数 → `sceneEnvironment`
- **`sceneEnvironment`** 分组存放管理器特有全局数据：环境光（ambient）、天空盒（skybox），不进入 ECS Registry
- **`entities[]`** 存放 ECS 实体数据（灯、相机、水面、网格物体等），Manager 通过 ECS view 按需收集
- JSON 格式：`sceneEnvironment.ambient` / `sceneEnvironment.skybox` 替代顶层的 `environment` / `skybox`
- 此语义边界已在 `SceneDescription.h` 的结构体定义中实现，是场景 JSON 文件格式的一部分

### 实体模板设计要点

- 不暴露原子 ECS 组件给用户（Cocos/Godot 模式下用户操作的是组合结果）
- 模板定义在 JSON 中（`Editor/Config/entity_templates.json`），无需改 C++ 代码即可扩展
- 新建实体时 Outliner 弹出 Godot 风格弹窗（分类树 + 搜索框）
- 模板 JSON 文档见 `Docs/architecture/EntityTemplates.md`

#### Outliner 实体模板弹窗遗留问题

| # | 问题 | 说明 |
|:-:|:-----|:------|
| 1 | **Water 模板缺少 WaterManager 注册** | `CreateEntityFromTemplate` 仅添加了 `WaterComponent` 标记，未调用 `WaterManager::RegisterWaveParams`。当前已改为 ECS 组件存储波浪参数（amplitude/frequency/speed/direction），需确认模板 JSON 中的 `water` 字段能否被 `OutlinerPanel::CreateEntityFromTemplate` 正确读取并写入组件 |
| 2 | **Mesh 模板未实现** | `CreateEntityFromTemplate` 中 mesh 分支被注释（`// mesh 暂不实现（需要从 AssetBrowser 选择具体网格/材质）`）。模板 JSON 中的 `mesh.geometry` / `mesh.material` 为空字符串，创建后无法渲染。后续需实现：① 模板创建时添加占位 MeshComponent；② 用户通过 AssetBrowser 拖拽网格/材质到实体上覆盖 |
| 3 | **缺少文档** | `Docs/architecture/EntityTemplates.md` 尚未创建，模板 JSON schema 和规则无文档说明 |

### 关系模型的约束

详见 `Docs/architecture/RelationshipModel.md`：

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
| 15 | **DataSlotPool 多池路由** | P2 | 当前 `poolId` 已编码在 `DataSlotHandle` 中（4 bits，支持 16 池），但实际所有调用都传 `0`。后续应根据数据类型（Mesh/Texture/Audio 等）使用不同 `poolId` 分配到不同池 |

### 预览系统

| # | 任务 | 优先级 | 说明 |
|:-:|:-----|:-------|:------|
| 16 | **缩略图磁盘缓存** | P2 | 当前缩略图仅存在于运行时内存（ThumbnailArray），关闭后丢失。需要：<br>• `BackgroundExecutor` 异步执行 `ReadbackSlice` + 写入磁盘<br>• 缓存文件使用独立后缀（如 `.thumb`），**禁止使用 `.dds`**<br>• 缓存目录：`Content/Cache/Thumbnails/`<br>• 启动时从磁盘加载缓存到 ThumbnailArray |
| 17 | **预览属性卡增强** | P3 | 当前已有 Orbit 相机控制（拖拽旋转 + 滚轮缩放）和光照参数滑块，缺：<br>• 程序化模型切换（球体/立方体/环面）<br>• 材质参数调整（颜色、粗糙度、金属度等） |
| 18 | **纹理预览兼容性** | P2 | 排查并补充预览系统支持的纹理格式列表 |
| 19 | **缩略图懒加载策略** | P3 | 启动时异步加载磁盘缓存，避免卡顿 |
| 20 | **预制体预览** | P3 | 双击 `.prefab` JSON 组合 Mesh + Material 渲染 |

### 编辑器资源隔离

| # | 任务 | 优先级 | 说明 |
|:-:|:-----|:-------|:------|
| 21 | **编辑器资源与游戏 Content 分离** | P3 | 编辑器资源 → `Editor/Content/`，游戏资源 → `Content/` |

### 渲染架构演进（2026-07-28 讨论定案）

| # | 任务 | 优先级 | 说明 |
|:-:|:-----|:-------|:------|
| 22 | **MeshComponent 材质槽数组化** | P2 | `materialHandle` 改为 `std::vector<MaterialHandle>`，支持 sub-mesh 级材质分配。参照 Cocos ModelComponent.materials[] 模式，submesh[i] → material[i]。向后兼容单材质场景。需同步更新：`MeshComponent` → `ExportToDescription`（JSON 支持 `materials[]`）→ `SceneConstructor` → `OpaqueRenderItemBuilder`（按 sub-mesh 拆分渲染项） |
| 23 | **MeshEditor 属性卡** | P2 | 注册 MeshComponent 的 ECS 属性卡编辑方法。功能：<br>• 显示当前网格名称/SubMesh 数量（只读）<br>• 材质槽列表（可替换，从 AssetBrowser 拖拽材质）<br>• `receivesShadow` 开关<br>• 后续：运行时替换 `lodMeshHandle`（从 AssetBrowser 拖拽网格） |
| 24 | **SubMesh 级材质替换** | P3 | 支持同一网格的不同 SubMesh 使用不同材质（如 NPR 角色身体 + PBR 衣物）。`MeshComponent.materialSlots.size()` 与 SubMesh 数量一致，`OpaqueRenderItemBuilder` 按 slot 拆分生成独立 RenderItem |

> 材质槽数组化后，场景 JSON 格式变化示例：
> ```json
> "mesh": {
>     "geometry": "character",
>     "materials": ["mat_body", "mat_hair", "mat_eyes"]  // 数组，与 submesh 数量一致
> }
> ```
> 向后兼容：旧格式 `"material": "mat_body"` 等效于 `"materials": ["mat_body"]`

### 低优先

| # | 任务 | 优先级 | 说明 |
|:-:|:-----|:-------|:------|
| 8 | 去掉 Shaders POST_BUILD 复制 | P1 | ShaderUtils 已接管 |
| 9 | 统一 `ResolvePath` 窄/宽字符 | P1 | M3dLoader 已删除，优先级降低 |
| 10 | 编辑器图标/资源 | P1 | `.rc` / `.ico` |
| 11 | 场景 JSON schema 更新 | P1 | `.ddsmesh` → `.dxmesh` |
| 12 | **PhysicsScene / AudioScene / NavMeshScene 子模块** | P3 | 按需添加，后续扩展 |
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

- `Docs/architecture/SceneManager.md` — 完整架构设计（含 §10 多 Tab 架构 + 事件流）
- `Docs/architecture/ComponentEditorSystem.md` — 组件驱动属性卡与 ImGuizmo 集成设计
- `Docs/architecture/InputSystem.md` — 输入系统架构（三层数据模型、TriggerBehavior 语义、数据流图）
- `Docs/architecture/EngineOverview.md` — 引擎架构总览（含 Game 端最新文件结构）
- `Engine/Scene/SceneManager.h` — 核心接口定义