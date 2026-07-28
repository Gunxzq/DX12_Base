# BugFix: ECS 光源收集模式与多 Tab 场景过滤

## 问题描述

将 `LightManager` 改造为从 ECS 收集光源数据（`CollectFromECS`）后，在多 Tab 编辑器模式下出现两个问题：

1. **光源跨 Tab 污染**：`CollectFromECS` 使用 `view<LightComponent, TransformComponent>()` 遍历 ECS Registry 中**所有**实体，不区分 `SceneTagComponent.sceneId`。当打开多个 Tab 时，每个 Tab 的光源同时生效，导致光照结果叠加错误。

2. **ApplyTabState 与 CollectFromECS 时序冲突**：Tab 切换时 `ApplyTabState` 从快照 `lightDescs` 恢复光源，但随后 `CollectFromECS` 覆盖为 ECS 数据。`lightDescs` 在 `OnSceneConstructReady` 中捕获的是**所有 Tab 的混合光源**，导致恢复结果错误。

## 根因分析

### 引擎 CORE 与编辑器端的职责差异

```cpp
// LightManager::CollectFromECS — 引擎 CORE 层
void LightManager::CollectFromECS(ECS::Registry *registry) {
    auto view = registry->view<LightComponent, TransformComponent>();
    for (auto entity : view) {
        // 收集 ALL 实体，无 sceneId 过滤 ← 引擎 CORE 不感知多 Tab
    }
}
```

`LightManager` 属于引擎 CORE 层，设计时未考虑编辑器多 Tab 场景。Game 端只有单一场景，所有实体天然属于当前世界，不需要 sceneId 过滤。但编辑器端多个 Tab 的实体共存于同一个 ECS Registry，需要通过 `SceneTagComponent.sceneId` 区分。

### 快照捕获的混合问题

`EditorSceneManager::OnSceneConstructReady` 在加载新场景时先捕获 `LightManager::GetLightCount/GetLight` 到 `snap.lightDescs`。但此时 LightManager 中的光源是上一帧 `CollectFromECS` 收集的**所有 Tab 的光源**，导致快照包含混合数据。`ApplyTabState` 恢复时误将其他 Tab 的光源应用到当前 Tab。

## 修复方案

### 1. CollectFromECS 添加过滤回调

```cpp
// LightManager.h — 引擎 CORE
void CollectFromECS(ECS::Registry *registry,
                    std::function<bool(ECS::Entity)> filter = nullptr);
```

引擎 CORE 提供过滤机制，编辑器端传入 `sceneId` 过滤回调，Game 端不传 filter 收集全部。

### 2. 编辑器端传入 sceneId 过滤

```cpp
// Editor.cpp — Immediate 回调
uint64_t activeSceneId = m_editorSceneMgr.GetActiveSceneId();
lightMgr->CollectFromECS(registry,
    [activeSceneId](ECS::Entity entity) -> bool {
        auto *tag = registry->TryGetComponent<SceneTagComponent>(entity);
        return tag && tag->sceneId == activeSceneId;
    });
```

### 3. ApplyTabState 移除 lightDescs 恢复

`ApplyTabState` 不再从 `snap.lightDescs` 恢复光源，仅恢复环境光。方向光/点光源/聚光源由 `CollectFromECS` 每帧从 ECS 按 sceneId 过滤收集。

## 影响范围

| 层级 | 改动文件 | 影响 |
|:-----|:---------|:------|
| 引擎 CORE | `LightManager.h/.cpp` | `CollectFromECS` 新增 `filter` 参数，签名变更 |
| 编辑器端 | `Editor.cpp` | 传入 sceneId 过滤回调 |
| 编辑器端 | `EditorSceneManager.cpp` | `ApplyTabState` 移除 lightDescs 恢复 |
| Game 端 | `Game.cpp` | 不传 filter，行为不变 |

## 经验教训

1. **引擎 CORE 与编辑器端的差异需要显式处理**：CORE 层提供通用的过滤/扩展机制（如回调、参数），编辑器端通过参数实现特定行为。Game 端使用默认值（不传 filter = 收集全部）。
2. **多 Tab 场景下的数据隔离**：所有从 ECS Registry 读取数据的逻辑（Builder、CollectFromECS 等）都必须按 `SceneTagComponent.sceneId` 过滤，否则 Tab 间数据相互污染。
3. **快照捕获的时序**：在 `Clear()` 之前捕获快照数据是最佳时机，但捕获的内容必须是当前 Tab 的独立数据，而非全局混合数据。
