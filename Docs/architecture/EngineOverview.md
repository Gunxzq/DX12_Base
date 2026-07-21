# 引擎架构总览

> 导航文档：各子系统概览、数据流、模块关系。
> 详细设计见 `Docs/architecture/` 下各自独立文档。

---

## 一、层级结构

```
Game/Editor (应用层)
  │  入口、配置、窗口、输入
  ├── GameWorld (游戏世界)
  │   ├── GameWorld.cpp          → Initialize / Update / Clear
  │   ├── GameWorld_Scene.cpp    → 场景物体创建（已基本迁移到 JSON）
  │   ├── GameWorld_Assets.cpp   → 资产加载（已基本迁移到异步管线）
  │   ├── GameWorld_Builder.cpp  → Builder 系统注册
  │   └── GameWorld_RenderSystems.cpp → 渲染系统注册
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
