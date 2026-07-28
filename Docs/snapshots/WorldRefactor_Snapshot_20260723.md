# 架构重构快照 — World 提取 + SceneManager 清理 + Game 端语义拆分

> 日期：2026-07-25
> 状态：✅ Game 端迁移完成，GameRenderPipeline 14 个 Register 方法已全部实现，旧文件已清理

---

## 一、已完成

### 1. World 提取（ECS 绝对源头）

| 文件 | 内容 |
|:-----|:------|
| `Engine/ECS/World.h/.cpp` | **新增** — ECS 绝对源头，持有 `ECS::Registry` |
| `Engine/Scene/SceneManager.h/.cpp` | `Initialize()` 接受 `World*`，`GetRegistry()` 委托给 World |
| `Engine/Boot/Bootstrap.h/.cpp` | 新增 `m_world`，先 `World::Initialize()` 再 `SceneManager::Initialize(&m_world)` |

### 2. SceneManager 遗留清理

**移除的接口：** `RegisterEntity()`、`PersistEntity()`/`UnpersistEntity()`/`IsPersistent()`、`GetAllEntities()`、`ProcessPendingChanges()`、`RegisterEntityPreRemoveCallback()`

**移除的成员：** `m_entities`、`m_persistentEntities`、`m_preRemoveCallbacks`

**最终 SceneManager 定位：** 场景序列化器 + 环境状态容器，不再承担实体管理、持久化、逻辑分区等职责。

### 3. 编辑器 UI 修复

| 修复 | 说明 |
|:-----|:------|
| 工具栏空 Tab 时隐藏 | `!m_editorSceneMgr.GetOpenTabs().empty()` 检查 |
| 双击场景环境光闪烁 | `OnSceneConstructReady` 开头提前 `ApplyTabState` |
| 已存在 Tab 仍重复加载 | `SwitchScene` 返回 `bool`，调用方跳过 |
| 工具栏位置跳动 | `DrawTabBar` 通过 `outImageMin`/`outImageMax` 显式返回图像位置 |

### 4. Game 端语义拆分

| 新模块 | 路径 | 职责 | 状态 |
|:-------|:------|:------|:------|
| `GameRenderPipeline` | `Game/Game/RenderPipeline/` | 构建器、渲染器、队列、系统注册 | ✅ 已创建 |
| `GameResources` | `Game/Game/Resources/` | 白纹理创建、ECS 组件预触 | ✅ 已创建 |
| `GameWorld`（简化） | `Game/Game/Scene/` | 仅保留 Update + 场景管理 + 子模块编排 | ✅ 已重写 |

### 5. 文档更新

| 文档 | 内容 |
|:-----|:------|
| `Docs/architecture/World.md` | **新增** — World 架构设计文档 |
| `Docs/architecture/SceneManager.md` | 新增 §12（World 提取 + SceneManager 降级） |
| `Docs/architecture/EngineOverview.md` | 修正 §8.7（消除多 World 矛盾，对齐单一 World 方向） |

---

## 二、当前状态

```
Engine CORE:
  World（ECS 绝对源头）
    ├─ ECS::Registry
    ├─ CreateEntity / DestroyEntity
    └─ GetRegistry()

  SceneManager（场景序列化器 + 环境状态容器）
    ├─ World* 引用
    ├─ SetSkybox / SetEnvironment
    ├─ PrepareSceneSwitch
    └─ GetRenderScene()

Game 端:
  GameWorld（世界主循环）
    ├─ GameSceneManager（场景生命周期）
    ├─ GameRenderPipeline（渲染管线）  ← 新建
    └─ GameResources（GPU 资源）       ← 新建

Editor 端:
  Editor（主入口）
    ├─ EditorSceneManager（多 Tab + 逻辑分区）
    ├─ EditorLayout（布局 + 面板）
    └─ EditorViewport（视口渲染）
```

---

## 三、待办

### P1 — 后续优化

| # | 任务 | 说明 |
|:-:|:-----|:------|
| 1 | **声明式输入处理** | 参考 Editor 端 `EditorCameraSystem` 模式，将 `GameInputHandler` 改为 `InputSystem::BindCallback()` 注册 |
| 2 | **GameContext 加 World*** | 可选：在 `GameContext` 中直接加 `World*` 指针，方便各模块直接访问 |
| 3 | **Editor 端 World 直接访问** | `EditorLayout.cpp` 等通过 `m_context->SceneMgr->GetRegistry()` 的调用改为 `m_context->SceneMgr->GetWorld()->GetRegistry()` |

### ✅ 已处理（原 P0/P2 项已在 07-25 前完成）

| # | 原任务 | 当前状态 |
|:-:|:-------|:---------|
| P0-1 | `GameWorld_*.cpp` 引用旧成员 | ✅ 旧文件已删除，所有构建器/渲染器访问走 `m_renderPipeline.GetXXX()` |
| P0-2 | GameRenderPipeline 系统注册实现 | ✅ `GameRenderPipeline.cpp` 有 14 个 Register 方法完整实现（1455 行） |
| P0-3 | GameWorld.cpp 调用适配 | ✅ 只调用 `m_renderPipeline.RegisterXXX()`，无重复旧调用 |
| P2-5 | GameWorld_*.cpp 完全迁移 | ✅ 旧文件 `GameWorld_Builder.cpp`/`GameWorld_RenderSystems.cpp` 等已删除 |
| P2-6 | GameContext 加 World* | 可选，待推进 |