# ImGuizmo 与 ImGui 版本 ABI 不匹配

> 日期：2026-07-22
> 状态：临时修复（源码补丁）

---

## 问题描述

ImGuizmo vcpkg 包（2024-05-29）的预编译库 `imguizmo.lib` 基于旧版 ImGui ABI 编译，而项目使用 ImGui **1.92.9 WIP（Docking 分支）**，两者在以下 API 上存在 ABI 不兼容：

- `ImDrawList::AddPolyline()` — 参数顺序从 `(..., flags, thickness)` 改为 `(..., thickness, flags)`
- `ImDrawList::AddRect()` — 同上
- `ImDrawList::PathStroke()` — 同上

此变更发生在 ImGui 1.92.8（2026-05-07）。

## 当前处理

将 ImGuizmo 源码（从 vcpkg buildtrees 缓存中提取）直接放入项目 `Engine/ThirdParty/imguizmo/`，作为 DX12EditorLib 的一部分从源码编译，同时修改了 8 处 `AddPolyline`/`AddRect` 调用参数顺序以匹配新 API。

## 长期方案

| 方案 | 说明 | 优先级 |
|:-----|:------|:------:|
| **A** 等待 vcpkg 发布新版 imguizmo | 等 imguizmo 上游更新后，vcpkg 包会适配新 ImGui API，届时切回 `find_package(imguizmo CONFIG REQUIRED)` 即可 | P3 |
| **B** 锁定 ImGui 版本 | 将项目 ImGui 回退到 1.92.7 以匹配 vcpkg imguizmo，但 Docking 分支功能可能丢失 | P3 |
| **C** 保持现状 | 源码编译，维护成本低，但需注意后续 imguizmo 上游更新时同步补丁 | 当前选择 |

## 触发条件

在编辑器中使用 ImGuizmo 功能时（选中实体 → Gizmo 操作），旧参数顺序导致 ImGui 断言失败：

```
Expression: ((flags & ImDrawFlags_InvalidMask_) == 0) && "Incorrect parameter. Did you swap 'thickness' and 'flags'?"
```

## 相关文件

- `Engine/ThirdParty/imguizmo/` — 从源码编译的 ImGuizmo
- `CMakeLists.txt` — 已将 `imguizmo::imguizmo` 替换为源码编译
- `Docs/architecture/editor/ComponentEditorSystem.md` — ImGuizmo 集成设计