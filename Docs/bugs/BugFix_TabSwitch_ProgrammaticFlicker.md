# 程序化 Tab 切换频闪

> 日期：2026-07-22
> 状态：📋 已知缺陷，待处理

---

## 问题描述

双击场景文件时自动切换到对应 Tab，会导致一帧的频闪（Tab 选中状态闪烁，视口短暂空白）。

## 原因

ImGui 的 `ImGuiTabItemFlags_SetSelected` 仅生效一帧，下一帧被 ImGui 内部状态覆盖，导致 `m_activeTabIndex` 与 ImGui 选中状态不一致。

```
Frame N: 用户双击场景文件
  └─ SwitchScene 创建新 Tab，设置 m_activeTabIndex = 2
  └─ DrawTabBar 已渲染完毕（frame 前半段）

Frame N+1: 频闪帧
  ├─ DrawTabBar: ImGui 显示 Tab 0 为选中（内部状态）
  ├─ Builder filter: 使用 m_activeTabIndex = 2 的 sceneId
  ├─ 新 Tab 无实体 → 视口空白
  └─ 用户看到 Tab 0 选中 + 视口空白 = 频闪

Frame N+2: 恢复正常
  ├─ 用户手动点击 Tab 2 → ProcessPendingTabSwitch 执行
  └─ 正常渲染
```

## 当前处理

- 已移除所有程序化 `SetSelected` 调用
- `SwitchScene` 已存在 Tab 路径直接 `return`，不修改 `m_activeTabIndex`
- Tab 切换仅支持用户手动点击
- 首个 Tab 自动激活（无 Tab 时）

## 后续方案

需要寻找 ImGui 标准程序化 Tab 切换方案：

1. `ImGui::SetTabItemSelected()`（较新版本 ImGui 提供）
2. 自定义 TabBar 实现，不依赖 ImGui 内部状态
3. 在 `OnFileDoubleClick` 中模拟用户点击 Tab 操作

## 关联

- `Docs/architecture/SceneSnapshot.md §10.2`
- `EditorSceneManager::DrawTabBar()`
- `EditorSceneManager::SwitchScene()`