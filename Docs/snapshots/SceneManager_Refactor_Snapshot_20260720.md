# 场景管理器重构快照 (2026-07-20)

> 从 Step 0-4 骨架 → 组合模式重构（EditorSceneManager/GameSceneManager）+ 加载职责分离

---

## 最终架构

### 组合模式（取代继承）

```
SceneManager（Engine CORE，Bootstrap 值成员）
    │
    ├── EditorSceneManager（组合包装）
    │     ├── SceneManager* m_sceneMgr  ← 被包装的核心管理器
    │     ├── 编辑器特有：EntityDesc 缓存 / 导出 / NewScene
    │     ├── RegisterSceneConstructSystem()  ← 注册系统响应事件
    │     └── GetDefaultSceneDescription()    ← 默认场景描述
    │
    └── GameSceneManager（组合包装，同模式）
          ├── SceneManager* m_sceneMgr  ← 被包装的核心管理器
          ├── RegisterSceneConstructSystem()  ← 注册系统响应事件
          └── LoadSceneAsync()          ← 异步场景加载
```

### 加载职责分离

```
场景描述（什么）──→ 加载编排（怎么加载）──→ 实体管理（生命周期）
     │                      │                       │
 EditorSceneManager    AssetBrowser           EditorSceneManager
 (GetDefaultSceneDesc)  (LoadSceneDescription)  (CreateEntity/RegisterEntity)
 
 GameSceneManager      GameSceneManager       GameSceneManager
 (GetDefaultSceneDesc)  (LoadSceneAsync)        (CreateEntity/RegisterEntity)
```

### ✅ 已完成

| 步骤 | 内容 | 状态 |
|:----:|:-----|:------|
| **0** | SceneManager 骨架：内部持有 `Registry`，受控实体 API | ✅ |
| **1a** | **EditorSceneManager 组合模式**：取消继承 `SceneManager`，改为包装 `SceneManager*`，提供 `GetSceneManager()`/`CreateEntity()`/`RegisterEntity()`/`GetRegistry()` 包装方法 | ✅ |
| **1b** | **EditorSceneManager 职责收窄**：移除 `LoadDefaultScene()`/`LoadSceneFile()`/`IsLoading()`/`m_sceneCtor`，不再负责加载编排 | ✅ |
| **1c** | **EditorSceneConstructSystem 迁移**：从 `Editor::Initialize()` 内联 lambda 移至 `EditorSceneManager::RegisterSceneConstructSystem()` 成员方法 | ✅ |
| **2a** | **GameSceneManager 创建**：组合模式，同 EditorSceneManager，提供 `RegisterSceneConstructSystem()`/`LoadSceneAsync()`/`OnSceneConstructReady()` | ✅ |
| **2b** | **GameWorld 改用 GameSceneManager**：`m_registry = context->Registry` → `m_gameSceneMgr.Initialize(context->SceneMgr, context)`；移除 `m_sceneConstructor`/`m_asyncLoadDelay`/`m_asyncScenePath` | ✅ |
| **3** | **Registry 所有权移入 SceneManager**：`GameContext::Registry` 已移除，Registry 归 SceneManager 内部所有 | ✅ |
| **4** | **SchedulerContext 清理**：`Registry` 字段已移除 | ✅ |
| **5a** | **默认场景加载剥离**：场景描述 `GetDefaultSceneDescription()` 归 `EditorSceneManager`，加载编排 `LoadSceneDescription()` 归 `AssetBrowser`，Editor 仅一行桥接 | ✅ |
| **5b** | **场景文件双击加载**：从 `Editor.cpp` 回调移至 `AssetBrowser::OnFileDoubleClick()` 内部 | ✅ |

### 🔄 当前架构

```
编辑器中场景加载流程：

Editor::Initialize()
  ├─ EditorSceneManager::Initialize(context->SceneMgr, context)
  ├─ EditorSceneManager::RegisterSceneConstructSystem()
  ├─ AssetBrowser::LoadSceneDescription(EditorSceneManager::GetDefaultSceneDescription())
  │     └─ SceneConstructor::LoadScene → 异步加载天空盒纹理/网格
  │           └─ GeneratorTaskCompleteEvent
  │                 └─ EditorSceneManager::OnSceneConstructReady()
  │                       ├─ CreateEntity + ConstructEntity + RegisterEntity
  │                       └─ SkyboxManager::SetSkybox()  ← 天空盒就绪
  └─ EditorViewport::Initialize()

游戏端场景加载流程：

GameWorld::Initialize()
  ├─ GameSceneManager::Initialize(context->SceneMgr, context)
  ├─ GameSceneManager::RegisterSceneConstructSystem()
  ├─ GameSceneManager::LoadSceneAsync("async_test.scene.json")
  │     └─ SceneLoader::LoadFromFile → SceneConstructor::LoadScene
  │           └─ GeneratorTaskCompleteEvent
  │                 └─ GameSceneManager::OnSceneConstructReady()
  │                       ├─ CreateEntity + ConstructEntity + RegisterEntity
  └─ ...
```

### 📁 关键文件结构

```
Engine/
  ├─ Scene/
  │   ├── SceneManager.h/.cpp          ← 基类，内部持有 Registry，公共 GetRegistry()
  │   └── SceneConstructor.h/.cpp       ← 场景加载器（生成器）
  ├─ Asset/IO/Loader/
  │   └── SceneLoader.h/.cpp            ← JSON 场景解析
  ├─ Boot/
  │   ├── Bootstrap.h/.cpp             ← SceneManager 值成员，模块化初始化
  │   └── GameContext.h                ← Scene::SceneManager *SceneMgr（无 Registry 字段）
  └── Scheduler/
      └── FrameDriver.h/.cpp           ← 无 ECS 引用，无 Registry 参数

Editor/EditorLib/
  ├─ Core/Editor.h/.cpp               ← EditorSceneManager 值成员 + AssetBrowser 编排
  ├─ Scene/EditorSceneManager.h/.cpp   ← 组合模式，包装 SceneManager*
  ├─ Panels/AssetBrowser.h/.cpp       ← 持有 SceneConstructor，编排场景加载
  ├─ Panels/OutlinerPanel.h/.cpp       ← 通过 SceneManager* 访问实体
  └─ Input/EditorViewportInput.h/.cpp  ← 通过 SceneManager* 获取 Transform

Game/Game/Scene/
  ├─ GameSceneManager.h/.cpp           ← 组合模式，包装 SceneManager*
  └─ GameWorld.h/.cpp                  ← 使用 GameSceneManager 替代直接 m_registry
```

### ⏳ 待办

| # | 任务 | 优先级 | 说明 |
|:-:|:-----|:-------|:------|
| 1 | **系统执行恢复** | P1 | 系统执行暂由空 lambda 占位，需 SceneManager 重新调度 |
| 2 | **移除 SchedulerContext 完整** | P2 | 删除结构体、`GetSchedulerContext`、`CameraManager` 改为 `m_context->MainTimer->GetDeltaTime()` |
| 3 | **子场景模块**（Step 5） | P3 | 按需将 Manager 访问收敛到子模块（RenderScene、PhysicsScene 等） |
| 4 | **旧 EditorScene 清理** | P3 | 已无人使用，可删除 `EditorScene.h/.cpp` |