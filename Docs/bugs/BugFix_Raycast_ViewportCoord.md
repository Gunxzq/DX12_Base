# BugFix: 视口射线检测坐标换算——使用视口相对坐标而非窗口绝对坐标

> 日期：2026-07-26
> 涉及文件：`EditorCameraSystem.cpp`、`EditorLayout.cpp`、`EditorGizmoSystem.cpp`

---

## 一、问题描述

编辑器 Select 模式下点击视口进行射线检测选中实体时，射线方向计算错误，点击位置与实际的选中结果存在固定偏移。具体表现为点击实体 A 时选中实体 B，或点击空白区域时误选中远处的实体。

## 二、根因分析

### 2.1 窗口坐标 vs 视口坐标

ImGui 渲染时视口图像并不一定在窗口的 `(0, 0)` 位置：
- ImGui Dockspace、面板边框、TabBar 等元素会在视口图像周围留出空间
- `GetMouseX()`/`GetMouseY()` 返回的是**窗口绝对坐标**
- `ScreenToRay()` 期望的是**视口相对坐标**（相对于视口图像左上角）

### 2.2 修复前的代码

```cpp
// 修复前：直接使用窗口绝对坐标
FRay ray = m_context->VisibleRaycaster->ScreenToRay(
    static_cast<float>(window.GetMouseX()),
    static_cast<float>(window.GetMouseY()),
    viewportW, viewportH);
```

`GetMouseX()` / `GetMouseY()` 返回窗口绝对坐标（如 `[120px, 340px]`），而 `ScreenToRay` 将输入解释为视口相对坐标（期望 `[0, 0]` 对应视口左上角）。当视口不在窗口左上角时，两者差异导致射线方向出现固定偏移。

## 三、修复

### 3.1 EditorLayout 暴露视口坐标

`EditorLayout` 已经记录了视口图像在窗口中的位置 `m_viewportMin`/`m_viewportMax`（屏幕绝对坐标）：

```cpp
// EditorLayout.cpp L374-377
m_viewportMin = ImVec2(xBase, yBase);
m_viewportMax = hasImage ? imageMax
                          : ImVec2(xBase + contentRegionAvail.x, yBase + contentRegionAvail.y);
```

通过 `GetViewportMin()`/`GetViewportMax()` 暴露给外部使用。

### 3.2 EditorCameraSystem 减掉视口偏移

```cpp
// EditorCameraSystem.cpp L188-201 —— 修复后
// 屏幕坐标 → 世界射线（使用视口相对坐标而非窗口绝对坐标）
float vpW = m_viewportMax.x - m_viewportMin.x;
float vpH = m_viewportMax.y - m_viewportMin.y;
if (vpW <= 0.0f || vpH <= 0.0f)
    return;

auto &window = *m_context->Window;
float mx = static_cast<float>(window.GetMouseX()) - m_viewportMin.x;  // ← 减掉视口偏移
float my = static_cast<float>(window.GetMouseY()) - m_viewportMin.y;  // ← 减掉视口偏移

m_context->VisibleRaycaster->UpdateCameraData(m_context->predictedCameraData);
FRay ray = m_context->VisibleRaycaster->ScreenToRay(mx, my,
                                                     static_cast<uint32_t>(vpW),
                                                     static_cast<uint32_t>(vpH));
```

关键改动：将窗口绝对坐标 `GetMouseX()`/`GetMouseY()` 减去 `m_viewportMin.x`/`m_viewportMin.y`，得到视口相对坐标后传入 `ScreenToRay`。

## 四、影响范围

| 模块 | 影响 | 说明 |
|:-----|:------|:------|
| `EditorCameraSystem.cpp` | ✅ 修复 | 射线检测坐标修正，加上视口偏移补偿 |
| `EditorLayout.h/.cpp` | ✅ 已有 | `m_viewportMin`/`m_viewportMax` 已暴露，修复只需使用它 |
| `EditorGizmoSystem.cpp` | ✅ 独立 | 视锥体绘制使用 `WorldToScreen()`，其内部用 `viewportMin` + 视口尺寸做投影，坐标源与射线检测一致，不冲突 |
| `VisibleRaycaster::ScreenToRay` | ❌ 未修改 | 该函数本身正确——问题出在调用方传入的坐标是窗口绝对坐标 |

### 4.1 世界空间→屏幕投影的一致性验证

`EditorGizmoSystem.cpp` 中的 `WorldToScreen()` 用于视锥体绘制，其坐标换算方式与射线检测的坐标源一致：

```cpp
// WorldToScreen 使用 viewportMin 作为基准偏移
ImVec2 WorldToScreen(worldPos, viewProj, vpMin, vpSize) {
    float sx = (clipX * 0.5f + 0.5f) * vpSize.x + vpMin.x;
    float sy = (1.0f - (clipY * 0.5f + 0.5f)) * vpSize.y + vpMin.y;
    return ImVec2(sx, sy);
}
```

- `WorldToScreen`：世界坐标 → 屏幕像素坐标（窗口绝对坐标，含 vpMin 偏移）
- `ScreenToRay`：屏幕坐标 → 射线（期望视口相对坐标）
- 两者在视口坐标上是对称的：一个加 vpMin，一个减 vpMin

## 五、相关代码位置

- `Editor/EditorLib/Core/EditorLayout.h` L94-95 — `GetViewportMin()` / `GetViewportMax()`
- `Editor/EditorLib/Core/EditorLayout.cpp` L374-377 — `m_viewportMin` / `m_viewportMax` 计算
- `Editor/EditorLib/Viewport/Systems/EditorCameraSystem.cpp` L188-201 — 射线检测坐标修正
- `Editor/EditorLib/Viewport/Systems/EditorCameraSystem.h` L40-44 — `SetViewportRect()`
- `Engine/Renderer/Core/VisibleRaycaster.cpp` — `ScreenToRay()` 实现

## 六、后续注意事项

- 任何新的屏幕坐标↔世界坐标的换算（拾取、拖拽、Gizmo 操作等）都必须使用视口相对坐标或明确处理 vpMin 偏移
- `EditorGizmoSystem` 的 `WorldToScreen` 和 `DrawGizmo` 接收的是 `viewportMin`/`viewportMax`，与射线检测使用同一套坐标源，无需额外修正
