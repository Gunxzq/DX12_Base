# InputConfig 的 modifier/requires 对鼠标键不生效

> 日期：2026-07-24
> 首次发现：编辑器工具快捷键（Q/W/E）与相机移动（WASDQE）的键位冲突处理

---

## 一、问题描述

在 `default_input.json` 中使用 `"modifier": "Mouse_Right"` 或 `"requires": "Mouse_Right"` 作为 `Move`/`MoveUp`/`MoveDown` 等动作的条件修饰键时，运行时**不生效**——即使不按住鼠标右键，WASDQE 仍然触发相机移动。

## 二、根因分析

### 2.1 解析层

输入配置加载器 `InputConfigLoader.cpp` 对不同的 JSON 结构使用不同的解析路径：

| JSON 结构 | 解析函数 | modifier 处理 | requires 处理 |
|-----------|---------|---------------|--------------|
| `{ "key": "Space", "modifier": "Mouse_Right" }` | `ParseBindingSource` | ✅ 第22行 | ✅ 第25行 |
| `{ "key": "Space", "requires": "Mouse_Right" }` | `ParseBindingSource` | — | ✅ 第25行 |
| `{ "keys": ["W","A",...], "modifier": "Mouse_Right" }` | `ExtractSourcesFromComplexItem` | ✅ 第94行 | ❌ **不处理** |

对 `keys` 数组形式（`Move` 使用），**只认 `"modifier"` 不认 `"requires"`**。

### 2.2 运行时层

即使解析正确地将 `ModifierKey` 设为 `EKeyCode::Mouse_Right`（1001），运行时检查仍然无效。问题在 `RawInputBuffer`：

```cpp
// InputSystem.cpp:121-122
if (rawBuffer.IsKeyDown(source.KeyCode) &&
    (source.ModifierKey == EKeyCode::None || rawBuffer.IsKeyDown(source.ModifierKey))) {
```

`rawBuffer.IsKeyDown` 检查 `m_keyStates[code]`。鼠标按键码（`Mouse_Left=1000`, `Mouse_Right=1001`）超出键盘键码范围，但 `m_keyStates` 数组大小支持到 `EKeyCode::Max`，所以索引访问本身不会越界。

**根本原因**：鼠标按键事件（`WM_RBUTTONDOWN` 等）的处理路径与键盘不同——`RawInputBuffer::OnKeyDown` 主要处理键盘消息，鼠标按钮的按下/释放状态可能未被正确写入 `m_keyStates[1001]`。

## 三、解决方案

### 3.1 最终采用方案

**代码级条件检查**替代输入配置的修饰键机制：

```cpp
// EditorCameraSystem.cpp — Move/MoveUp/MoveDown 回调
if (!m_inputSystem->IsActionHeld(ActionId_OrbitCamera))
    return;
```

`IsActionHeld(ActionId_OrbitCamera)` 通过 InputSystem 的 action 状态追踪来判断 RMB 是否按住，不受 RawInputBuffer 鼠标键码追踪缺陷影响（`Look` 回调一直用此方式，经长期验证有效）。

### 3.2 保留的配置层防御

`default_input.json` 仍保留 `"modifier": "Mouse_Right"` 作为辅助过滤，即使当前运行时层不生效，若后续修复 RawInputBuffer 鼠标键追踪后可自动起作用。

## 四、影响范围

| 模块 | 影响 |
|------|------|
| `InputConfigLoader.cpp` | keys 数组模式下 `requires` 被忽略（设计缺陷） |
| `RawInputBuffer.h` | 鼠标按键状态未追踪到 `m_keyStates`（设计缺陷） |
| `EditorCameraSystem.cpp` | Move/MoveUp/MoveDown 回调添加 `IsActionHeld(OrbitCamera)` 检查 |
| `EditorViewportToolbar.cpp` | 不受影响，继续使用 `IsMouseDown(1)` 检查 |

## 五、相关文件

- `Editor/Config/default_input.json` — `modifier: "Mouse_Right"` 保留
- `Editor/EditorLib/Viewport/Systems/EditorCameraSystem.cpp` — 代码级检查
- `Engine/Platform/Input/InputConfigLoader.cpp` — 解析器实现
- `Engine/Platform/Input/RawInputBuffer.h` — 按键状态缓冲区
