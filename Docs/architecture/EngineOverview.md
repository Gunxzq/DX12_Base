# 引擎架构总览

> 导航文档：各子系统概览、数据流、模块关系。
> 详细设计见 `Docs/architecture/` 下各自独立文档。

---

## 一、层级结构

```
Game/Editor (应用层)
  │  入口、配置、窗口、输入
  ├── GameWorld (游戏世界 — 主循环 + 子模块编排)
  │   ├── GameWorld.cpp              → Initialize / Update / Clear
  │   ├── GameSceneManager           → 场景生命周期管理
  │   ├── GameRenderPipeline         → 构建器/渲染器/队列 + 系统注册
  │   │   └── GameRenderPipeline.cpp → 全部 16 个系统注册方法
  │   └── GameResources              → GPU 资源初始化（白纹理、预触）
  │
  └── Game::Run() → FrameDriver::Tick()
       ├── Immediate 回调（LightManager/WaterManager 上传）
       ├── PreRender（Builder 系统，Worker 线程并行）
       ├── Render（事件驱动 + 场景构造）
       └── PostProcess（渲染系统消费队列）

Engine/ (引擎层)
  ├── Core/             → 路径系统、项目配置、SharedDataStore
  ├── Resource/         → GPU 资源管理、资产缓存、纹理/几何体管理器
  │   └── AssetManager/ → 异步资产加载（注册表模式待实施）
  ├── Background/       → 后台任务执行器、LoadTask (Mesh/Texture)
  ├── Renderer/         → 渲染管线、场景管理器、构建器、RHI
  ├── Scene/            → SceneConstructor（JSON → ECS 编排器）
  ├── ECS/              → entt 组件/注册表
  ├── Event/            → 消息分发
  └── Scheduler/        → FrameDriver、TaskGraph
```

---

## 二、核心数据流

### 场景加载（异步）

```
async_test.json
  ↓ SceneLoader::LoadFromFile
SceneDescription
  ↓ SceneConstructor::LoadScene
AssetManager::LoadBatch(meshes + textures)
  ↓ 后台线程
MeshLoadTask + TextureLoadTask (cpuWork → gpuWork → onComplete)
  ↓ 全部完成
OnDependenciesLoaded
  ├─ 纹理 SRV 映射
  ├─ 材质注册
  ├─ SkyboxManager::SetSkybox()
  ├─ 材质 buffer GPU 上传（同步 submit + flush）
  └─ SceneConstructData → SharedDataStore
       ↓ PostEvent(GeneratorTaskCompleteEvent, payload=genType<<32|sceneId)
SceneConstructSystem → ConstructEntity → ECS 实体
```

### 渲染帧

```
FrameDriver::Tick()
  │
  ├─ Immediate 回调
  │   ├─ CameraManager::UpdateMainCamera()
  │   ├─ LightManager::UpdateAndUpload()
  │   └─ WaterManager::UpdateAndUpload()
  │
  ├─ PreRender（Worker 线程并行）
  │   ├─ BuilderUpload（串行，设 Frustum/Camera）
  │   ├─ BuildOpaque / BuildTransparent / BuildWater / ...
  │   └─ FrameSync（串行，分配 RingBuffer）
  │
  ├─ Render
  │   ├─ 场景构造（事件驱动）
  │   └─ 阴影/SSAO
  │
  └─ PostProcess
      ├─ 不透明渲染
      ├─ 水渲染
      ├─ 天空盒渲染
      └─ 透明渲染
```

---

## 三、子系统关系

```
                     Scene JSON
                         │
                    SceneConstructor
                    ┌────┴────┐
              SkyboxManager  AssetManager
                    │         │
                    │    MeshLoadTask / TextureLoadTask
                    │         │
                    │    BackgroundExecutor
                    │         │
                    │    onComplete → 注册到各 Manager
                    │
              ┌─────┴────────────────────────────────┐
              │                                       │
       SkyboxManager  WaterManager  LightManager   MaterialMgr
       (单例,SRV+CB)  (RingBuffer)  (RingBuffer)   (GPU Buffer)
              │              │              │              │
              └──────┬───────┴──────┬───────┘              │
                     │              │                      │
               SkyboxRender    WaterRender            OpaqueRender
               System          System                 System
                     │              │                      │
               SkyRenderer     WaterRenderer          OpaqueRenderer
```

---

## 四、ECS → Builder → Renderer 模式

```
ECS Components                    Builders (PreRender)       Renderers (PostProcess)
─────────────────                ────────────────────       ─────────────────────────
MeshComponent      ───→  OpaqueRenderItemBuilder   ───→  OpaqueRenderer
  + OpaqueTag
                          TransparentRenderItemBuilder  →  (预留)
MeshComponent
  + TransparentTag
                          WaterRenderItemBuilder       ───→  WaterRenderer
WaterComponent
  + MeshComponent
  + TransparentTag
                          TerrainRenderItemBuilder     ───→  TerrainRenderer
TerrainComponent
                          SkinnedRenderItemBuilder     ───→  SkinnedRenderer
MeshComponent
  + SkinnedTag

非 ECS：

SkyboxManager (单例)                                   ───→  SkyRenderSystem
WaterManager (单例)                                     ───→  WaterRenderer::BeginFrame
```

---

## 五、管理器一览

| 管理器 | 单例 | 自管资源 | 上传时机 | 用途 |
|:-------|:-----|:---------|:---------|:------|
| `SkyboxManager` | ✅ | SRV + UPLOAD CB | 场景加载时 SetSkybox | 环境贴图 |
| `WaterManager` | ✅ | RingBuffer | Immediate 回调 | 波浪数据 |
| `LightManager` | ✅ | RingBuffer × 5 | Immediate 回调 | 光源 + 阴影 |
| `TerrainManager` | ✅ | — | Immediate 回调 | 地形常量 |
| `MaterialManager` | ✅ | GPU Buffer | SceneConstructor onComplete | 材质数据 |
| `TextureManager` | ✅ | SRV 堆 | 异步加载 onComplete | 纹理注册 |
| `GeometryResourceManager` | ✅ | VB/IB 池 | 异步加载 onComplete | 网格注册 |
| `AmbientOcclusionManager` | ✅ | 内部 RT | Initialize | SSAO |
| `ReflectionProbeManager` | ✅ | Cubemap 数组 | Initialize | 反射探针 |

---

## 六、重要设计决策

| 决策 | 说明 | 文档 |
|:-----|:------|:------|
| 天空盒不经过 ECS | 全局属性，Manager 直接持有 | `SkyboxManager` |
| 水体走 ECS + Manager | 复数实体 + 全局波浪模拟 | `WaterSystemArchitecture.md` |
| 材质 buffer 异步创建 | SceneConstructor onComplete 中分配 | `AsyncPipelineResponsibilities.md` |
| 公告牌无 MeshComponent | Sprite/GS 渲染，不经过 Opaque 管线 | `BillboardSystemArchitecture.md` |
| 异步加载 Allocator fence 值 | `Acquire` 用 `GetCompletedFenceValue`，`Release` 用 `GetNextSequence` | `AllocatorAndEnttFixes.md` |
| entt 存储池主线程预触 | 避免 Worker 线程首次 assure() 竞态 | `AllocatorAndEnttFixes.md` |

---

## 七、参考文档导航

| 文档 | 内容 |
|:-----|:------|
| `AsyncPipelineResponsibilities.md` | 异步加载管线角色职责 |
| `WaterSystemArchitecture.md` | 水系统架构（Manager + ECS + Builder） |
| `BillboardSystemArchitecture.md` | 公告牌三种方案 |
| `SceneFileAndLoading.md` | 场景文件格式与加载流程 |
| `AssetLoaderImprovement.md` | AssetManager 注册表改进方案 |
| `FrameResourceManager.md` | RingBuffer / FrameResource 管理 |
| `Frame.md` | 帧生命周期、Phase 顺序 |
| `RenderDataAccess.md` | 渲染数据访问模式 |
| `AllocatorAndEnttFixes.md` | 本次修复的 bug 记录 |

---

## 八、Editor/Game 端差异：ECS-Builder-Renderer 管道

> 2026-07-23 补充：Editor 和 Game 端共用同一套 ECS-Builder-Renderer 管道，但通过不同的可见性策略满足各自需求。

### 8.1 大型引擎参考

#### Unity

```
Unity 渲染管线（SRP/URP/HDRP）：
  Editor 和 Game 共用同一套管线代码，无分支

  Editor 额外渲染（选中轮廓、Gizmo）：
    └─ 通过 Handles.DrawCamera + CommandBuffer 注入
    └─ 不是另一套管线，而是在现有管线中插入额外 Pass

  可见性控制：
    └─ Camera.cullingMask（按 Layer 过滤）
    └─ SceneView 使用独立 Camera，走自己的 cullingMask
    └─ 不修改 Builder，不修改 Renderer，只配置 Camera 的可见性

  核心原则：
    └─ 管线不分叉，视图配置决定可见性
```

#### Unreal

```
Unreal 渲染场景（FScene）：
  Editor 和 Game 共用同一个 FScene，无分叉

  Editor 额外渲染：
    └─ FSceneView 的 ShowFlags 控制
    └─ ShowFlags.SetEditorPrimitives() / SetGameplay() / ...
    └─ 视口按需开关，不修改底层 FScene

  可见性控制：
    └─ 每个 FEditorViewportClient 有自己的 ShowFlags 集合
    └─ 同一批 Actor 进入不同视口，ShowFlags 决定哪些可见
    └─ 不修改 Builder，不修改 World，只配置视口的显示标志

  核心原则：
    └─ 场景不分叉，视口配置决定可见性
```

#### 对我们的启示

| 维度 | Unity/Unreal 的做法 | 我们当前的做法 |
|:-----|:--------------------|:--------------|
| **管道路径** | 同一套管线，不分叉 | 同一套 Builder-Renderer，方向正确 |
| **可见性控制** | 视口/Camera 级别（cullingMask / ShowFlags） | **Builder 级别（EntityFilter 提前过滤）** |
| **编辑器叠加** | 额外 Pass 注入现有管线（CommandBuffer / ShowFlags） | 未实现 |
| **场景数据** | 所有实体在同一场景，视口配置决定可见性 | 通过 SceneTagComponent 在 Builder 层过滤 |

**关键差异：** Unity/Unreal 不在 Builder 层过滤实体。Builder 处理所有实体，渲染决策推迟到视口/Camera 级别。这意味着同一批实体可以被不同视口以不同方式渲染（主场景显示 gameplay、编辑器视口显示辅助线）。

### 8.2 当前方案的局限

```
Builder 层过滤的问题：
  └─ 过滤掉的实体无法被编辑器叠加渲染使用
  └─ 例如：选中实体的轮廓高亮需要实体进入渲染队列
  └─ 例如：Gizmo 需要目标实体的深度信息
  └─ 过滤策略是"有或无"的二元选择，不够灵活

更灵活的方式：
  └─ Builder 处理所有可见实体（不过滤）
  └─ 渲染时由视口/Camera 配置决定可见性
  └─ 编辑器叠加渲染作为额外 Pass 注入
```

### 8.3 Editor 端差异

```
Editor 端：
  ECS Registry（全局数据容器，多源头）
    ├─ SceneManager → 场景实体（用户编辑的关卡）
    ├─ PreviewSystem → 预览实体（缩略图、材质球）
    ├─ DebugSystem → 调试可视化（碰撞体、光线）
    └─ 各系统各行其是，互不干扰

  Builder 策略：
    └─ 处理所有带 RenderMeshComponent 的实体
    └─ 不过滤，不提前剔除

  视口配置（EditorViewport）：
    └─ 拥有自己的 Camera 实例
    └─ 通过可见性标志控制渲染内容
    └─ 叠加编辑器专用 Pass：
          ├─ 选中实体高亮轮廓
          ├─ ImGuizmo
          └─ 调试可视化

  SceneTagComponent 用途：
    └─ 不用于 Builder 过滤
    └─ 用于场景序列化（保存场景时知道哪些实体属于哪个场景）
    └─ 用于 UI（Outliner 按场景分组显示）
```

### 8.4 Game 端差异

```
Game 端：
  ECS Registry（全局数据容器，单一世界）
    ├─ SceneManager → 关卡实体（场景加载）
    ├─ ParticleSystem → 粒子实体（运行时生成）
    ├─ AnimationSystem → 动画状态实体
    ├─ PhysicsSystem → 碰撞体实体
    └─ 所有游戏内容共享同一个世界

  Builder 策略：
    └─ 处理所有带 RenderMeshComponent 的实体
    └─ 不过滤

  视口配置：
    └─ 主相机控制可见性
    └─ 无编辑器叠加 Pass
    └─ 无调试可视化
```

### 8.5 管道可定制性

ECS-Builder-Renderer 管道是**统一且可定制的**，不是两套代码：

```
统一管道：
  ECS Registry → 组件查询 → 构建渲染项 → 提交渲染

定制点：
  └─ 视口/Camera 配置（可见性、叠加 Pass）
       ├─ Editor：SceneTagComponent 过滤 + 编辑器叠加 Pass
       └─ Game：不过滤，无叠加 Pass
```

### 8.6 设计原则

1. **ECS-Builder-Renderer 管道是统一的**，Editor 和 Game 共用同一套基础设施
2. **可见性控制应在视口/Camera 级别**，不在 Builder 级别
3. **编辑器叠加渲染作为额外 Pass 注入**，不修改主渲染管线
4. **SceneTagComponent 用于序列化，不用于渲染过滤**

---

### 8.7 World 重构：ECS 的绝对源头

> 2026-07-23 补充：大型引擎的启示——引擎核心应有唯一的绝对 ECS 源头（World），SceneManager 降级为场景序列化器，双端特化在 World 层完成。

#### 8.7.1 问题

当前 `SceneManager` 职责过重：它既是 ECS 实体管理容器，又是场景文件加载器，又被 Editor 和 Game 各自特化。但 `ECS::Registry` 作为全局数据容器并不属于 `SceneManager`——预览系统、调试系统都在往 Registry 里写实体，`SceneManager` 没有能力也不应该管理它们。

#### 8.7.2 大型引擎参考

```
Unity：
  SceneManager（场景加载/卸载）
    └─ 只管理场景文件的生命周期
    └─ 所有 GameObject 属于某个场景，但场景不管理 GameObject 的运行时行为
  World 概念：
    └─ 隐式存在（SceneManager 内部维护场景层级）
    └─ DontDestroyOnLoad 对象存在于"持久场景"

Unreal：
  UWorld（绝对 ECS 源头）
    ├─ 所有 Actor 属于某个 UWorld
    ├─ ULevel（场景/关卡）是 UWorld 的子集
    └─ 多个 UWorld 可共存（EditorWorld、PreviewWorld、PIEWorld）
  UWorld 是绝对容器，ULevel 是序列化单元
```

#### 8.7.3 新架构：单一 World + 逻辑分区

```
Engine Core（绝对 ECS 源头）：
  World（单一实例）
    ├─ 持有 ECS::Registry
    ├─ 所有实体都必须属于这个 World
    ├─ 实体生命周期入口（CreateEntity / DestroyEntity）
    └─ World 本身不分区，不感知任何逻辑分组

  逻辑分区（由 Manager 的解释器视角提供）：
    ├─ SceneManager 的视角：
    │     └─ 查询所有带 SceneTagComponent 的实体
    │     └─ 按 sceneId 分组 → 多 Tab 场景管理
    │     └─ 负责这些实体的序列化/反序列化
    │
    ├─ PreviewManager 的视角：
    │     └─ 查询所有带 PreviewTag 的实体
    │     └─ 管理预览实体的创建/销毁
    │     └─ 不参与场景保存
    │
    ├─ DebugManager 的视角：
    │     └─ 查询所有带 DebugTag 的实体
    │     └─ 管理调试可视化实体的创建/销毁
    │     └─ 不参与场景保存
    │
    └─ 其他 Manager 同理
          └─ 每个 Manager 通过 TagComponent 查询自己的"逻辑分区"

Editor 端：
  └─ 单一 World，多个 Manager 提供多个逻辑分区

Game 端：
  └─ 单一 World，无逻辑分区
  └─ 所有实体都是"游戏内容"，不需要 Tag 区分
  └─ SceneManager 只做关卡加载/卸载
```

#### 8.7.4 核心变化

```
之前（多个 World 实例）：
  EditorWorld（场景实体）→ 独立 Registry
  PreviewWorld（预览实体）→ 独立 Registry
  DebugWorld（调试实体）→ 独立 Registry
  └─ 问题：跨 World 的实体交互困难（如选中预览实体）
  └─ 问题：渲染管线需要多个 World 的渲染项合并

现在（单一 World + 逻辑分区）：
  World（唯一的 Registry）
    ├─ 场景实体 ← SceneManager 的视角
    ├─ 预览实体 ← PreviewManager 的视角
    ├─ 调试实体 ← DebugManager 的视角
    └─ World 不分区，Manager 提供视角

  优势：
    ├─ 跨分区交互简单（选中预览实体 = 选中同一 World 的实体）
    ├─ Builder 不需要合并多个 World 的渲染项
    ├─ 渲染管线不需要区分"来自哪个 World"
    └─ Game 端不需要任何分区概念
```

#### 8.7.5 SceneManager 的降级

```
SceneManager（不再管理 ECS，只做场景序列化 + 环境状态管理）：
  LoadScene(path) → 创建实体到 World
  SaveScene(path) → 从 World 中查询场景实体 → 写入文件
  └─ 不再持有 ECS::Registry 引用
  └─ 不再管理实体生命周期（委托给 World）
  └─ 逻辑分区是 Editor 端特化，不属于 Engine Core

  Editor 端特化（EditorSceneManager）：
    ├─ 多 Tab 管理（多个场景文件同时打开）
    ├─ Tab 切换 → 切换 sceneId 查询条件
    ├─ 异步加载 + 竞态保护
    ├─ 编辑器状态持久化
    └─ SceneTagComponent 逻辑分区

  Game 端特化（GameSceneManager）：
    ├─ 单场景加载/卸载
    ├─ 关卡切换过渡
    └─ 不需要 SceneTagComponent（所有实体都是游戏内容）
```

#### 8.7.6 迁移路径

| 步骤 | 内容 | 影响 |
|:----:|:-----|:------|
| 1 | 从 Engine Core 中提取 `World` 类，持有 `ECS::Registry` | 新增，不破坏现有代码 |
| 2 | 将 `SceneManager::CreateEntity`/`RemoveEntity` 委托给 `World` | 接口兼容，内部重定向 |
| 3 | 将 `ECS::Registry` 的全局访问改为通过 `World::GetRegistry()` | 逐步替换 |
| 4 | `SceneManager` 降级为场景序列化器，不再持有 Registry | 重构，需验证 |
| 5 | Editor 端各 Manager 通过 TagComponent 提供逻辑分区视角 | 渐进式 |

#### 8.7.7 设计原则

1. **World 是 ECS 的绝对源头**，所有实体必须属于某个 World
2. **SceneManager 是场景序列化器**，不是 ECS 容器，也不是逻辑分区解释器
3. **单一 World + 逻辑分区（Editor 端）**，非多 World 实例
4. **Game 端无分区概念**，所有实体都是游戏内容
5. **World 和 SceneManager 是组合关系**，SceneManager 持有 World* 引用

> 详细设计见 `Docs/architecture/World.md`。

---

## 九、ECS 统一数据源与 Manager 的分工

> 2026-07-27 补充：ECS 是运行时唯一数据源，Manager 降级为 GPU 打包器。

### 9.1 问题

当前部分 Manager（如 LightManager）持有私有数据源：

```
LightManager（当前）
  ├─ m_lights[] ← 私有数组
  ├─ 不与 ECS 打通
  └─ 属性卡无法编辑、剔除系统无法感知
```

属性卡基于 ECS 驱动，Manager 的私有数据既不可编辑也无法参与剔除/射线检测。而渲染管线需要连续 GPU buffer，ECS 组件天然不满足这个需求——需要在 ECS 和 GPU 之间有一层打包转换。

### 9.2 分层

```
ECS Registry（唯一运行时数据源）
  ├─ LightComponent + TransformComponent  ← 定义"这里有灯光"
  ├─ CameraComponent + TransformComponent ← 定义"这里有相机"
  ├─ WaterComponent                        ← 定义"这里有水面"
  └─ ... 其他场景实体数据
        │
        ▼ System 每帧读取
Manager（聚合器 + GPU 打包器）
  ├─ LightManager
  │     ├─ 从 ECS View 收集可见光源
  │     ├─ 打包为连续 GPU light buffer
  │     └─ 管理 shadow map 数组
  ├─ CameraManager
  │     ├─ 从 isMain CameraComponent 读取参数
  │     └─ 计算 view/proj 矩阵
  └─ WaterManager
        ├─ 从 WaterComponent 读取波浪参数
        └─ 上传到 GPU CB
```

### 9.3 角色重定义

| 角色 | 数据持有 | 职责 | 生命周期 |
|:-----|:---------|:-----|:---------|
| **ECS 组件** | 实体属性（位置、灯光参数） | 定义"有什么"，参与剔除/拾取/编辑 | 实体的增删改 |
| **Manager** | 无私有数据源 | 读取 ECS → 聚合 → 打包 GPU buffer | 引擎启动到关闭 |
| **System** | 不持有数据 | 在 ECS 和 Manager 之间做转换 | 每帧执行 |

### 9.4 过渡

当前 LightManager 持私有 `m_lights[]` 数组。过渡路径：

```
阶段一（现状）：SceneConstructor 同时写入 ECS + Manager
阶段二（目标）：SceneConstructor 只写 ECS，LightManager::Update 从 ECS 读取打包
```

此过渡可以渐进完成，先验证 CameraComponent 模式（从 ECS 驱动 CameraManager），再将同样模式推广到 LightManager、WaterManager。

#### 实施状态（2026-07-28）

| Manager | 收集方法 | 状态 |
|:--------|:---------|:------|
| **CameraManager** | 从 `isMain` CameraComponent 读取参数 | ✅ 已有 |
| **LightManager** | `CollectFromECS(registry)` 遍历 `LightComponent + TransformComponent` | ✅ 已实施 |
| **WaterManager** | `CollectFromECS(registry)` 遍历 `WaterComponent`，提取波浪参数 | ✅ 已实施 |

**调用时序**：

```
Immediate 回调每帧：
  LightManager::CollectFromECS(registry)
  WaterManager::CollectFromECS(registry)
  ↓
  LightManager::UpdateAndUpload(fence, camera)
  WaterManager::UpdateAndUpload(fence)
```

**SceneConstructor** 变更：`LightComponent` 创建从 TODO 变为完整实现；`WaterComponent` 波浪参数直接写入 ECS，不再调用 `WaterManager::RegisterWaveParams`。

### 9.5 判断准则

新模块是否适合 ECS + Manager 模式：

```
需要做 ECS 组件？
  ├─ 数据是否对应现实场景中的"物体"（灯、相机、水面）？ → 是 → ECS
  └─ 数据是否单一全局参数（环境光强度、雾密度）？   → 可单例，可 ECS

需要做 Manager？
  ├─ 多个实体的数据需要聚合为连续 GPU buffer？ → 需要 → Manager
  ├─ 数据管理 shadow map / 纹理数组等 GPU 资源？  → 需要 → Manager
  └─ 仅简单的参数传递 → 不需要 Manager，System 直接上传
```

### 9.6 SceneEnvironment 语义边界

> 2026-07-28 补充：`SceneEnvironment` 作为管理器特有全局数据与 ECS 实体数据的显式语义分隔。

并非所有场景数据都能放入 ECS 实体。场景 JSON 中通过 `sceneEnvironment` 字段（参见 `SceneDescription.h`）明确区分两类数据：

```
SceneDescription
  ├─ sceneEnvironment（管理器全局数据，不进入 ECS Registry）
  │     ├─ ambient：环境光（→ LightManager::SetAmbientLight）
  │     └─ skybox：天空盒（→ SkyboxManager::SetSkybox）
  │     └─ (未来扩展：fog、postProcess、timeOfDay 等)
  │
  └─ entities（ECS 实体数据，写入 ECS Registry）
        ├─ 有 TransformComponent → 场景中的"物体"
        ├─ 参与剔除/拾取/编辑/序列化
        └─ Manager 从 ECS view 收集 → 打包 GPU buffer
```

**分界线的判断规则**：

| 条件 | 归属 | 示例 |
|:-----|:-----|:------|
| 有位置/变换，可独立编辑 | `entities[]` (ECS 组件) | 灯、相机、水面、网格物体 |
| 无实体身份，全局参数 | `sceneEnvironment` | 环境光颜色、天空盒纹理、雾密度、后处理设置 |

**典型不在 entities 中的数据**（Manager 特有，不参与剔除/拾取/编辑）：

| 数据 | 管理方 | 理由 |
|:-----|:-------|:------|
| 环境光 (ambientLight) | LightManager | 全局光照参数，无位置 |
| 天空盒 (texture/geometry/color) | SkyboxManager | 全局背景，无实体身份 |
| 雾密度/颜色（未来） | FogManager | 全局视觉效果 |
| 后处理（未来） | PostProcessManager | 全局画面调色 |

**JSON 示例**：

```json
{
  "version": 1,
  "sceneEnvironment": {
    "ambient": { "ambientLight": [0.25, 0.25, 0.35, 1.0] },
    "skybox": { "texture": "sky_cubemap", "geometry": "skybox_mesh" }
  },
  "entities": [
    { "name": "Sun", "components": { "transform": {...}, "light": {...} } },
    { "name": "MainCamera", "components": { "transform": {...}, "camera": {...} } }
  ]
}
```

**设计要点**：

1. **`sceneEnvironment` 中的数据不进入 ECS Registry**，直接由 SceneConstructor 传递给对应的 Manager
2. **`entities` 中的数据全部写入 ECS**，Manager 通过 ECS view 按需读取收集
3. `materials`（内联材质定义）和 `dependencies`（资产依赖）不属于任何一个分类——它们是场景序列化的辅助元数据，不直接对应运行时状态
4. 此语义边界写入 `SceneDescription.h` 的结构体定义中，是 JSON 文件格式的一部分
