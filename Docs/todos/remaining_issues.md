# 全局待办清单

> 日期：2026-07-17
> 本次会话：OutlinerPanel 独立、NameComponent、FocusSelection、输入上下文重构。

---

## 一、当前已验证可工作

| 系统 | 状态 | 说明 |
|:-----|:------|:------|
| 异步管线（AssetManager + BackgroundExecutor） | ✅ | Mesh/Texture/Material 异步加载 |
| SceneConstructor（JSON → ECS） | ✅ | skybox/ground/opaque/water 实体 |
| SkyboxManager | ✅ | Cubemap SRV + UPLOAD CB，独立于 FrameDriver |
| WaterManager | ✅ | RingBuffer 自管上传 + 波浪参数 |
| Opaque 渲染 (JSON 实体) | ✅ | 标准 ECS → Builder → Renderer |
| Water 渲染 (JSON 实体) | ✅ | WaterRenderItemBuilder → WaterRenderSystem |
| 地形渲染 | ✅ | 保留 |
| 阴影/SSAO/反射探针 | ✅ | 保留不受影响 |

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
| 5 | 多堆域 SRV 隔离（SkyboxManager → LightManager 等） | P2 | SkyboxManager 已支持 `HeapTag` 参数，SRV 创建在指定堆域。LightManager、ReflectionProbeManager 等需同样扩展，使其在多堆（Editor）模式下不占用 Default 堆域空间 |
| 5 | AssetManager 注册表模式 | P2 | 见 `AssetLoaderImprovement.md`，用文件后缀分发替代 `switch(type)` |
| 6 | TerrainLoadTask 拆分为并行子 Task | P1 | 几何体 + 纹理各走独立 Task |
| 7 | 编辑器中启用渲染管线 | P2 | 当前骨架 `return 0` |
| 8 | **渲染管线清除职责分离** | P2 | 当前清除散落在各渲染 System 内部，导致重复清除和职责混杂。重构方向：<br>• **Editor**: 注册独立 `EditorClearSystem`（`alwaysRun`，`RenderPhase::PrePass`），负责清除 EditorViewport 的离屏 RT + depth，设置视口。`EditorSkyboxRenderSystem` 和 `EditorGridRenderSystem` 只渲染、不清除。<br>• **Game**: GameWorld 的 PrePass（清 backbuffer+depth）、GBuffer（清 G-buffer）、Skybox（清 backbuffer+depth）存在多次重复清除，需统一到 PrePass 阶段的一个 ClearSystem。<br>• Game 和 Editor 是独立 exe，互不影响，各自按需实现。见 `BugFix_EditorViewport_ClearValueMismatch.md` 和相关讨论 |
| 9 | **Bootstrap 职责分离：不初始化非 Default 堆域分区** | P2 | 当前 `Bootstrap::InitializeModules()` 在 `isEditor && Multi` 模式下为所有非 Default `HeapTag` 创建了 `Texture/Buffer/Shadow` 分区。但 Bootstrap 属于引擎核心层，不应感知编辑器特定的堆域布局。正确设计：<br>• Bootstrap 只负责选择 `HeapMode::Multi`（基于 `ProjectConfig::Type`）<br>• 非 Default 堆域的分区由各 Editor 模块（如 `EditorViewport`）在自身初始化时注册<br>• 当前为临时方案，后续需将 Bootstrap 中 `isEditor && Multi` 块内的 `AddPartition` 循环移出，交由 Editor 各模块自行管理 |
| 10 | **SkyboxManager 自带几何体生成** | P2 | 当前 `SkyboxManager` 依赖 `SceneConstructor` 从外部加载立方体网格（`cube.dxmesh`）作为天空盒渲染几何体。改造方向：<br>• `SkyboxManager` 内部通过 `GeometryGenerator` 生成立方体（或球体）的 VB/IB，直接注册到 `GeometryResourceManager`<br>• 消除对 `cube.dxmesh` 外部文件的依赖<br>• 场景描述中 `skybox.geometry` 变为可选，默认使用内部生成几何体<br>• 简化 `EditorScene::LoadDefaultScene()` 和 JSON 场景的 skybox 配置 |
| 11 | **编辑器布局重构** | P2 | 当前 `EditorLayout` 直接管理所有面板的创建和绘制，存在耦合。改造方向：<br>• 布局只作为大体分块框架：左列（Outliner/AssetBrowser）、中列（Viewport）、右列（Properties）<br>• 每列再分为上下两部分（如左列上半 Outliner、下半 AssetBrowser）<br>• 面板通过注册回调的方式挂载到布局框架，不再由 `EditorLayout` 直接创建<br>• 每个面板独立管理自己的可见性和状态，布局只负责分配位置和尺寸 |
| 12 | **网格比例尺控件** | P2 | 当前水平滑条不符合习惯，改为垂直刻度尺样式，贴合传统 3D 编辑器的网格比例尺设计 |

### 共享数据中心

| # | 任务 | 优先级 | 说明 |
|:-:|:-----|:-------|:------|
| 13 | **DataSlotPool 多池路由** | P2 | 当前 `poolId` 已编码在 `DataSlotHandle` 中（4 bits，支持 16 池），但实际所有调用都传 `0`。后续应根据数据类型（Mesh/Texture/Audio 等）使用不同 `poolId` 分配到不同池，各池可配置独立的 Arena 大小和 TLS 批量策略。句柄的 `poolId` 位段确保从任意池分配的句柄都可统一寻址，无需额外映射。配置见 `Editor/Config/` 下的池配置 |

### 预览系统

| # | 任务 | 优先级 | 说明 |
|:-:|:-----|:-------|:------|
| 14 | **缩略图磁盘缓存** | P2 | 当前缩略图仅存在于运行时内存（ThumbnailArray），关闭后丢失。需要：<br>• `BackgroundExecutor` 异步执行 `ReadbackSlice` + 写入磁盘<br>• 缓存文件使用独立后缀（如 `.thumb`），**禁止使用 `.dds`**，避免被资产系统当作纹理加载<br>• 缓存目录放在项目根目录下（`Content/Cache/Thumbnails/`），而非编辑器目录——因为缩略图是项目粒度的，不同项目各自独立<br>• 启动时从磁盘加载缓存到 ThumbnailArray<br>• 可考虑打包为单个二进制文件（类似项目配置），避免散文件管理 |

### 编辑器资源隔离

| # | 任务 | 优先级 | 说明 |
|:-:|:-----|:-------|:------|
| 15 | **编辑器资源与游戏 Content 分离** | P3 | 当前 iconfont（`Content/Fonts/iconfont.ttf`）等编辑器专属资源混在游戏 `Content/` 目录下。分离后：<br>• 编辑器资源 → `Editor/Content/`（iconfont、编辑器图标等）<br>• 游戏资源 → `Content/`（网格、纹理、材质、场景等）<br>• 缩略图缓存属于项目粒度，放在 `Content/Cache/` 下，不属于编辑器资源<br>• 路径解析由 Bootstrap 根据 `ProjectConfig::Type` 区分<br>• 编辑器和 Game 独立 exe 后各自只加载自己的资源目录 |

### 低优先

| # | 任务 | 优先级 | 说明 |
|:-:|:-----|:-------|:------|
| 8 | 去掉 Shaders POST_BUILD 复制 | P1 | ShaderUtils 已接管 |
| 9 | 统一 `ResolvePath` 窄/宽字符 | P1 | M3dLoader 已删除，优先级降低 |
| 10 | 编辑器图标/资源 | P1 | `.rc` / `.ico` |
| 11 | 场景 JSON schema 更新 | P1 | `.ddsmesh` → `.dxmesh` |

---

## 三、GameWorld::Initialize() 当前状态

```
Initialize()
  ├─ Water / Sky / Shadow / Terrain 渲染器初始化
  ├─ Opaque / Transparent / Terrain / Probe / Water / Skinned 构建器初始化
  ├─ 白色纹理创建（反射探针测试用）
  ├─ // LoadSoldierCharacter（已注释）
  ├─ // LoadBillboardTextures / CreateBillboardTrees（已删除）
  ├─ // CreateMaterials / CreateSkybox / CreateGroundPlane / CreateWater（已删除）
  ├─ entt 预触（9 个组件类型）
  └─ SceneConstructor 异步加载 async_test.json（JSON 驱动全部场景内容）
```

---

## 四、核心文件结构

```
Engine/
  ├─ Asset/
  │   └── IO/Loader/
  │       ├── SceneLoader.h/.cpp        ← JSON 场景解析
  │       ├── DxMeshLoader.h/.cpp        ← .dxmesh 二进制加载
  │       └── M3dLoader.h/.cpp          ← 已删除
  ├─ Background/
  │   ├── MeshLoadTask.h                ← 异步 Mesh 加载
  │   ├── TextureLoadTask.h              ← 异步 Texture 加载
  │   └── BackgroundExecutor.h/.cpp      ← 后台调度引擎
  ├── ECS/Core/Components/
  │   ├── Water.h                       ← 水体组件
  │   ├── Render.h / Tags.h / Transform.h
  │   └── Skybox.h                      ← 已删除
  ├── Renderer/
  │   ├── Scene/
  │   │   ├── SkyboxManager.h/.cpp      ← 天空盒管理器
  │   │   ├── WaterManager.h/.cpp        ← 水管理器
  │   │   └── LightManager/              ← 光源管理器
  │   ├── RenderItemBuilder/
  │   │   ├── WaterRenderItem.h/.cpp     ← 水构建器
  │   │   └── Billboard*               ← 已删除
  │   └── RHI/Command/Allocator/
  │       └── CommandAllocatorPool.h/.cpp ← 修复后
  └── Scene/
      └── SceneConstructor.h/.cpp        ← 场景编排器

Game/Game/Scene/
  ├── GameWorld.h/.cpp                   ← 主入口
  ├── GameWorld_Scene.cpp                ← 场景物体创建
  ├── GameWorld_Assets.cpp               ← 资产加载
  ├── GameWorld_Builder.cpp              ← Builder 注册
  └── GameWorld_RenderSystems.cpp        ← 渲染系统注册
```

---

## 五、场景管理器渐进式迁移

### 背景

`Engine/Scene/SceneManager.h/.cpp` 骨架已创建（Step 0），尚未投入使用。以下为逐步迁移路径，每一步独立可验证。

### 迁移步骤

| Step | 内容 | 文件 | 验证 |
|:----:|:-----|:-----|:-----|
| **0** | SceneManager 骨架：内部持有 `Registry`，受控实体 API，`GetRegistryForInternalUse()` 向后兼容 | `Engine/Scene/SceneManager.h/.cpp` | ✅ 已完成 |
| **1** | **EditorScene → EditorSceneManager**：替换 `EditorScene`，SceneConstructSystem 改为 `CreateEntity + RegisterEntity` 流程，缓存 EntityDesc 用于导出 | `Editor/EditorLib/Scene/` | ✅ 已完成 |
| **3** | **Registry 所有权移入 SceneManager**：移除 `GameContext::Registry`，FrameDriver 不再持有 ECS 引用，SceneManager 改为 Bootstrap 值成员 | `Engine/Boot/Bootstrap.cpp`、`Engine/Scheduler/FrameDriver.h` | ✅ 已完成 |
| **4** | **SchedulerContext 清理**：移除 `registry` 字段，`InitializeSchedulerContext` 不再接收 Registry 参数，系统函数签名改为 `void(const MessageContext &)` | `Engine/Scheduler/FrameDriver.h/.cpp` | ✅ 已完成 |
| **2** | **GameWorld → GameSceneManager**：移除 `GameWorld::m_sceneConstructor`，改为通过 `GameSceneManager` 统一管理 | `Game/Game/Scene/` | ⏳ 待做 |
| **5** | **系统执行恢复**：TaskGraphBuilder 中系统执行由 SceneManager 重新调度 | `Engine/Scheduler/TaskGraphBuilder.cpp` | ⏳ 待做 |
| **6** | **移除 SchedulerContext 完整**：删除结构体、`GetSchedulerContext`、`CameraManager` 改为直接访问 | `Engine/Scheduler/FrameDriver.h/.cpp`、`Engine/Renderer/Scene/CameraManager.cpp` | ⏳ 待做 |
| **7** | **子场景模块**：按需将 Manager 访问收敛到子模块（RenderScene、PhysicsScene 等） | 各子模块文件 | ⏳ 待做 |

### 依赖关系

```
Step 0（骨架）
  │
  ├──→ Step 1（EditorSceneManager）──→ Step 3（Registry 所有权迁移）
  │                                         │
  │                                         ├──→ Step 4（SchedulerContext 清理）
  │                                         │
  └──→ Step 2（GameSceneManager）───────────┘
                                                  │
                                                  ├──→ Step 5（系统执行恢复）
                                                  ├──→ Step 6（SchedulerContext 完整移除）
                                                  └──→ Step 7（子场景模块）
```

### 相关文档

- `Docs/architecture/SceneManager.md` — 完整架构设计
- `Engine/Scene/SceneManager.h` — 核心接口定义
