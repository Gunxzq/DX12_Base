# PreviewRenderEvent 多条同类型消息丢失导致预览不渲染

## 症状

编辑器 Detail 预览（独立 RT 模式）不渲染：日志显示 `PreviewManager::DispatchPreviewRenderEvents` 发出了两个 `PreviewRenderEvent`（Thumbnail + Detail），但 `TestPreviewRenderer` 只收到了第一个（Thumbnail），Detail 事件被静默丢弃。

## 根因

`TaskGraphBuilder::BuildTasks()` 中，遍历消息列表创建 Task 时，存在一条系统 ID 去重逻辑：

```cpp
// TaskGraphBuilder.cpp (原第 164 行)
if (systemToTask.find(sysId) != systemToTask.end()) {
    continue;  // ← 同类型第二条消息直接跳过！
}
```

当同一帧内有**多条同类型消息**（例如两个 `PreviewRenderEvent`，分别携带 id=2 和 id=3 的 payload）时：
1. 第一条消息（id=2）为 `TestPreviewRenderer` 创建了 Task，`systemToTask[sysId] = tid`
2. 第二条消息（id=3）发现 `TestPreviewRenderer` 已在映射中 → **跳出**，不创建 Task

结果是该 System 始终只处理第一条消息，后续同类型消息全部丢失。

## 修复

**方案**：允许为同一 System 针对多条消息创建多个 Task，每个 Task 捕获各自的消息上下文。

### 改动

#### `TaskGraphBuilder.h`
- `BuildTasks` 返回类型：`unordered_map<SystemId, TaskId>` → `unordered_map<SystemId, vector<TaskId>>`
- `BuildDependencies` 参数同步

#### `TaskGraphBuilder.cpp`
- **BuildTasks**：移除消息触发的 System 的去重跳过（`continue`），每条消息独立创建 Task，使用 `systemToTask[sysId].push_back(tid)`
- **BuildDependencies**：嵌套循环处理 `vector<TaskId>`，为当前系统的每个 Task 分别建立与依赖系统每个 Task 的边
- 修复 `depId` 变量名遮蔽

## 涉及文件

| 文件 | 改动 |
|:----|:-----|
| `Engine/Scheduler/TaskGraphBuilder.h` | `BuildTasks`/`BuildDependencies` 签名更新为 `vector<TaskId>` |
| `Engine/Scheduler/TaskGraphBuilder.cpp` | 移除去重；支持多 Task 创建与依赖建立 |

## 经验教训

1. **消息驱动的调度器**中，同类型多条消息应独立下发到 System，不能按 System ID 去重。去重逻辑应仅适用于 `alwaysRun` 常驻 System，以"系统是否存在"而非"是否已创建 Task"为判断依据
2. 调试日志 + 发布日志双模式有利于快速定位此类问题：`DispatchPreviewRenderEvents` 日志清楚显示两个事件都已发出，但 `TestPreviewRenderer` 日志只显示收到一个
