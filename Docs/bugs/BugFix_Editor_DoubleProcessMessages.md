# BugFix: Editor 主循环双重 `ProcessMessages` 导致鼠标 Delta 丢失（右键旋转失效）

## 日期

2026-07-15

## 症状

1. **右键拖拽旋转相机完全无响应**：日志中 `[EditorViewportInput] OrbitCamera action held` 正确输出，确认右键事件被输入系统接收，但画面视角不变化。
2. **WASD 移动有效但"极其异常"**：WASD 能移动相机位置，但方向始终沿初始45度俯视角（Forward = (0, -0.707, 0.707)），无法通过旋转改变视角。
3. **鼠标滚轮缩放无效**（同样机理）。

## 根因

`Editor::Run()` 中**额外调用了一次** `Window->ProcessMessages()`，导致鼠标 delta 数据在每帧被重置为 0：

### 数据流追踪

```
Editor::Run() 主循环:
┌─────────────────────────────────────────────────────────────────────┐
│ [1] Window->ProcessMessages()          ← ① 处理消息，RawInputBuffer  │
│     OnMouseMove(x, y) 累计鼠标 delta     累计鼠标 delta（如 ΔX=100）  │
│                                                                     │
│ [2] MainTimer->Tick()                                              │
│ [3] BackgroundExecutor->Tick()                                     │
│ [4] FrameDriver::Tick()                                            │
│     ├─ InputMgr->BeginFrame()          ← ② 重置 m_mouseDeltaX = 0  │
│     │   RawInputBuffer::BeginFrame():      ⚠️ 鼠标 delta 丢失！     │
│     │     m_mouseDeltaX = 0;                                        │
│     │     m_mouseDeltaY = 0;             ← 按键状态不受影响          │
│     │     m_mouseWheelDelta = 0;                                    │
│     ├─ Window->ProcessMessages()        ← ③ 队列已空，无新消息       │
│     ├─ InputMgr->Update()               ← ④ 读取鼠标 delta = 0     │
│     │   EvaluateActions:                                            │
│     │     Look: rawBuffer.GetMouseDeltaX() = 0                     │
│     │     Look: rawBuffer.GetMouseDeltaY() = 0                     │
│     └─ ExecuteImmediate()               ← ⑤ 相机更新回调            │
│         EditorViewportInput::Update()                               │
│           GetActionAxis2D(Look) = (0, 0)                           │
│           Pitch(0), RotateY(0) → 画面不动                          │
└─────────────────────────────────────────────────────────────────────┘
```

### 为什么 WASD 不受影响

`RawInputBuffer::BeginFrame()` **只重置鼠标增量数据**，不重置按键状态：

```cpp
void BeginFrame() {
    m_mouseDeltaX = 0;     // ⚠️ 鼠标 delta 被清 0
    m_mouseDeltaY = 0;     // ⚠️
    m_mouseWheelDelta = 0; // ⚠️
    // m_keyStates[] 不受影响  ← 所以 WASD 按键状态保留
}
```

按键状态在 `OnKeyDown`/`OnKeyUp` 中设置，`BeginFrame` 不清除，因此 WASD 的 `IsKeyDown` 查询结果正确。但移动方向依赖 Forward/Right 向量，由于右键旋转失效，相机始终朝初始方向（45度俯角）移动，表现为"异常"。

### 对比 Game 端（正常）

```
Game 主循环:
  MainTimer->Tick()
  WorldUpdate()
  FrameDriver->Tick()          ← 唯一的一次 ProcessMessages 在内部
    ├─ InputMgr->BeginFrame()  ← 重置 delta
    ├─ Window->ProcessMessages() ← 唯一处理消息，delta 正确
    ├─ InputMgr->Update()      ← 读取正确 delta
    └─ ...
```

Game 端没有额外的 `ProcessMessages()` 调用，所以数据流是完整的。

## 约束规则

**`Window::ProcessMessages()` 必须在每帧**只被调用一次**，且只能在 `FrameDriver::Tick()` 内部由 `FrameDriver` 自身调用。**

- `FrameDriver::Tick()` 内部已经包含 `BeginFrame()` → `ProcessMessages()` → `InputMgr->Update()` 的完整输入处理链路
- 任何在 `FrameDriver::Tick()` 外部的 `ProcessMessages()` 调用都会导致 `BeginFrame()` 将已处理的消息数据（特别是鼠标 delta）清空
- 按键状态虽不受影响，但鼠标增量数据（delta / wheel）会丢失

## 正确模式

```cpp
// ✅ 正确：Editor 主循环
while (...) {
    m_context->MainTimer->Tick();
    m_context->BackgroundExecutor->Tick();
    m_context->FrameDriver->Tick();  // 内部处理窗口消息 + 输入
    // 其他逻辑
}

// ❌ 错误：不要在 FrameDriver::Tick() 之外调用 ProcessMessages()
while (...) {
    m_context->Window->ProcessMessages();  // ← 禁止！
    m_context->MainTimer->Tick();
    ...
    m_context->FrameDriver->Tick();
}
```

## 修复

`Editor::Run()` 中删除多余的 `m_context->Window->ProcessMessages()` 调用（原第421行）。

## 参考

- `Editor/EditorLib/Core/Editor.cpp` — `Editor::Run()` 主循环
- `Engine/Scheduler/FrameDriver.cpp` — `FrameDriver::Tick()` 内部输入处理顺序
- `Engine/Platform/Input/RawInputBuffer.h` — `BeginFrame()` 实现
- `Engine/Platform/Windows/Window.cpp` — `ProcessMessages()` 实现
- `Game/Game/Game.cpp` — Game 端主循环（正确示例）