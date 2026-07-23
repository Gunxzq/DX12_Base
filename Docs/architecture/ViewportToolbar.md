# 视口工具栏设计

> 参考 Cocos Creator / Unity 编辑器的视口左上角工具栏，为编辑器视口增加工具模式切换能力
>
> **实现状态**：已实现工具栏 UI、工具模式切换（QWE）、View/Select 子模式切换、图标字体集成。
> **待解决**：View 模式左键拖拽平移相机的效果不符合期望（屏幕空间上下映射到 Z 轴前后移动，效果不直观）。Select 模式射线检测因场景系统迁移时被注释，暂不可用。

---

## 一、设计目标

1. **工具栏** — 在视口左上角叠加 ImGui 工具栏，显示当前工具模式
2. **工具模式切换** — 点击工具栏按钮或按快捷键切换工具
3. **输入上下文分离** — 不同工具模式下的鼠标/键盘行为不同，输入系统根据当前模式路由事件
4. **可扩展** — 后续可添加更多工具（缩放、矩形选择、地形编辑等）

---

## 二、工具模式定义

### 2.1 初始工具集

| 图标 | 名称 | 快捷键 | 功能 |
|:----:|:-----|:-------|:-----|
| 🖱 | **Cursor（光标）** | `Q` | 鼠标交互模式，含两个子模式 |
| ┣━ 👁 View | — | 平移/旋转相机（无选中操作） |
| ┗━ 🎯 Select | — | 点击射线检测选中实体 |
| ✚ | **Translate（平移）** | `W` | 显示 Gizmo 平移控件，拖拽修改实体的 Position |
| 🔄 | **Rotate（旋转）** | `E` | 显示 Gizmo 旋转控件，拖拽修改实体的 Rotation |

### 2.2 工具模式枚举

```cpp
enum class ViewportTool : uint8_t {
    Cursor,     // 光标（含 View / Select 子模式）
    Translate,  // Gizmo 平移
    Rotate,     // Gizmo 旋转
    // Scale,   // 后续
    // Rect,    // 后续
    Count
};
```

### 2.3 Cursor 子模式

```cpp
enum class CursorMode : uint8_t {
    View,   // 平移/旋转相机
    Select  // 射线检测选中实体
};
```

Cursor 默认处于 `View` 子模式。当鼠标悬停在实体上时，可点击切换到 `Select` 模式选中实体。

---

## 三、声明式输入系统设计

### 3.1 设计动机

当前输入处理的问题：

| 问题 | 说明 |
|:-----|:------|
| **集中式** | `EditorViewportInput` 是中央集权的单体类，硬编码所有相机控制逻辑 |
| **非声明式** | 输入绑定分散在 `default_input.json` 中，与使用输入的实际 System 分离 |
| **不易扩展** | 每增加一个工具模式，就要在 `EditorViewportInput::Update()` 中增加 if-else 分支 |
| **不直观** | 读代码时看不到一个 System 需要什么输入，要在多个文件之间跳转 |

### 3.2 核心设计：声明式 System 输入

**让 System 在注册时声明自己需要的输入绑定，而不是在中央调度器中硬编码。**

```
声明式绑定（初始化时注册）
  │
  ├─ EditorCameraSystem 声明：
  │     ├─ "Move"        → WASD           → 移动相机
  │     ├─ "Look"        → MouseDelta     → 旋转相机
  │     ├─ "OrbitCamera" → MouseRight     → 启用轨道旋转
  │     └─ "Zoom"        → MouseWheel     → 缩放
  │
  ├─ EditorSelectSystem 声明：
  │     └─ "SelectClick" → MouseLeft      → 射线检测选中
  │
  └─ EditorToolbarSystem 声明：
        ├─ "ToolCursor"  → Q              → 切换光标工具
        ├─ "ToolTranslate" → W            → 切换平移工具
        └─ "ToolRotate"    → E            → 切换旋转工具
```

### 3.3 帧时序与推送边界

当前 `FrameDriver::Tick()` 的实际执行顺序：

```
L91:  InputMgr::BeginFrame()         ← 清空增量数据
L96:  Window::ProcessMessages()      ← 收集窗口消息
L103: InputMgr::Update(deltaTime)    ← 评估 Action 状态 ★
L110: DAG 构建
L158: ExecuteImmediate()             ← 回调读取已评估的 Action 状态
L167: ExecutePhase(Render)           ← 渲染 System 录制命令列表
L191: ExecutePhase(Update)           ← 逻辑更新 System
L209: FrameSync()                    ← 数据冻结
```

关键约束：
1. **InputMgr::Update() 在 ExecuteImmediate() 之前**——Action 状态在 Immediate 回调前已评估完毕
2. **Immediate → Render**——Immediate 回调之后紧跟着 Render Phase，**此时不能修改 ECS 组件**
3. **ECS 组件的修改只能在 Update Phase 进行**

推送式回调在 `InputSystem::Update()` 末尾触发，仍在 Immediate 阶段，不经过调度器：

```
InputMgr::Update()
  ├─ InputSystem::Update(rawBuffer, enabledActions, deltaTime, currentTime)
  │     ├─ EvaluateActions()       ← 评估所有 Action 的当前状态
  │     ├─ PerformEdgeDetection()  ← 边沿检测（Pressed/Released）
  │     └─ InvokeCallbacks()       ← ★ 推送：遍历注册表，触发回调
  │
  └─ （回调已执行完毕，回到 FrameDriver::Tick()）
        └─ ExecuteImmediate()
              └─ 这里也可以做轮询读取，但推送式已经在 Update() 中完成了
```

### 3.4 回调边界规则

| 操作 | 允许在 Immediate 回调中 | 说明 |
|:-----|:----------------------|:------|
| 写 `CameraMgr::Position/Forward/Up` | ✅ | `CameraMgr` 不是 ECS 组件，是全局状态 |
| 入队 `PendingPickRequest` | ✅ | 只存请求，不做精确检测 |
| 更新 ImGuizmo 内部状态 | ✅ | ImGuizmo 自有状态，非 ECS |
| 切换工具模式 | ✅ | 修改 `ViewportTool` 枚举，非 ECS |
| **写 `TransformComponent`** | ❌ | Render Phase 可能正在读 |
| **写 ECS 任何组件** | ❌ | 只能在 Update Phase 由调度器管理 |

### 3.5 InputSystem 扩展

#### 回调注册表

```cpp
// InputSystem.h 新增
struct InputCallback {
    ActionId actionId;
    std::function<void(const InputActionState&)> callback;
    InputDeclaration::TriggerBehavior trigger; // 可选：限制触发时机
};

using ActionCallbackId = uint32_t;

class InputSystem {
public:
    ActionCallbackId BindCallback(ActionId actionId,
        std::function<void(const InputActionState&)> callback,
        InputDeclaration::TriggerBehavior trigger = InputDeclaration::OnPressed);
    void UnbindCallback(ActionCallbackId id);

private:
    // 新增：回调表
    struct CallbackEntry {
        ActionCallbackId id;
        ActionId actionId;
        std::function<void(const InputActionState&)> callback;
        InputDeclaration::TriggerBehavior trigger;
    };
    std::vector<CallbackEntry> m_callbacks;
    ActionCallbackId m_nextCallbackId = 1;
};
```

#### 推送触发

```cpp
// InputSystem::Update() 末尾
void InputSystem::InvokeCallbacks() {
    for (auto& entry : m_callbacks) {
        const auto& state = m_actionStates[entry.actionId];
        // 根据 trigger 类型判断是否触发
        switch (entry.trigger) {
        case InputDeclaration::WhileHeld:
            if (state.GetHeld()) entry.callback(state);
            break;
        case InputDeclaration::OnPressed:
            if (state.GetPressed()) entry.callback(state);
            break;
        case InputDeclaration::OnReleased:
            if (state.GetReleased()) entry.callback(state);
            break;
        case InputDeclaration::Axis2D:
            // 轴类型：只要值变化就触发
            if (state.GetAxis2D().X != 0.0f || state.GetAxis2D().Y != 0.0f)
                entry.callback(state);
            break;
        }
    }
}
```

### 3.6 SystemInfo 扩展

在 `SystemInfo` 中增加输入声明字段，让 System 在注册时自带输入绑定：

```cpp
// SystemTypes.h 新增
struct InputDeclaration {
    std::string actionName;                         // 动作名称
    ActionId actionId;                              // 运行时 Hash
    std::vector<BindingSource> bindings;            // 默认绑定

    enum TriggerBehavior : uint8_t {
        WhileHeld,      // 按住时持续（如 Move）
        OnPressed,      // 按下时触发一次（如 Jump）
        OnReleased,     // 释放时触发（如 SelectClick）
        Axis2D,         // 二维轴（如 Look, Move）
    };
    TriggerBehavior trigger = OnPressed;
};

// SystemInfo 扩展
struct SystemInfo {
    // ... 现有字段不变 ...

    // ── 新增：输入声明 ──
    std::vector<InputDeclaration> inputDeclarations;
    std::vector<std::string> inputContexts;  // 哪些上下文下此 System 的输入生效
};
```

### 3.7 声明式注册示例

```cpp
// 在 EditorViewport 的 System 注册中，直接声明输入需求
SystemBuilder("EditorCameraSystem", PreUpdate, Render)
    .AlwaysRun()
    .Func([this](const MessageContext&) {
        // 相机控制：每帧轮询动作状态（如果不想用推送回调）
        auto& input = m_context->InputMgr->GetInputSystem();
        // 或者直接读取 CameraMgr——Immediate 回调已经写好了
    })
    .WithInputDeclarations({
        { "Move",        { Key('W'), Key('S'), Key('A'), Key('D') }, Axis2D },
        { "Look",        { MouseAxis(X), MouseAxis(Y) },             Axis2D },
        { "OrbitCamera", { MouseButton(Right) },                     WhileHeld },
        { "Zoom",        { MouseWheel() },                           Axis2D },
    })
    .WithInputContexts({ "Viewport" })
    .Build();
```

### 3.8 工具模式与输入上下文

工具模式切换本质上就是**输入上下文的切换**。复用现有的 `InputContextStack` 机制：

```
工具模式          → 输入上下文          → 启用的 Action
─────────────────────────────────────────────────────────
Cursor-View      → "Viewport_CursorView"  → Move, Look, OrbitCamera, Zoom, Pan
Cursor-Select    → "Viewport_CursorSelect" → Move, Look, OrbitCamera, Zoom, SelectClick
Gizmo-Translate  → "Viewport_Gizmo"       → Move, Look, OrbitCamera, Zoom
Gizmo-Rotate     → "Viewport_Gizmo"       → Move, Look, OrbitCamera, Zoom
```

切换工具时：

```cpp
void EditorViewportToolbar::SetCurrentTool(ViewportTool tool) {
    m_currentTool = tool;
    auto* ctxStack = m_context->InputMgr->GetContextStack();
    ctxStack->Clear();  // 弹出所有视口上下文
    switch (tool) {
    case ViewportTool::Cursor:
        ctxStack->PushContext(m_cursorMode == CursorMode::View
            ? "Viewport_CursorView" : "Viewport_CursorSelect");
        break;
    case ViewportTool::Translate:
    case ViewportTool::Rotate:
        ctxStack->PushContext("Viewport_Gizmo");
        break;
    }
}
```

### 3.9 输入上下文路由

```
Viewport 鼠标事件
  │
  ├─ 当前上下文 = Viewport_CursorView
  │     ├─ 左键拖拽 → Pan 动作 → 平移相机
  │     ├─ 右键拖拽 → OrbitCamera 动作 → 旋转相机
  │     └─ 滚轮     → Zoom 动作 → 缩放
  │
  ├─ 当前上下文 = Viewport_CursorSelect
  │     ├─ 左键点击 → SelectClick 动作 → 射线检测 → 选中实体
  │     ├─ 右键拖拽 → OrbitCamera 动作 → 旋转相机
  │     └─ 滚轮     → Zoom 动作 → 缩放
  │
  └─ 当前上下文 = Viewport_Gizmo
        ├─ 左键拖拽 Gizmo → ImGuizmo 内部处理
        ├─ 右键拖拽 → OrbitCamera 动作 → 旋转相机
        └─ 滚轮     → Zoom 动作 → 缩放
```

共性规则（所有上下文共享）：
- **右键拖拽（OrbitCamera）** 始终可用
- **鼠标滚轮（Zoom）** 始终可用
- **WASD（Move）** 始终可用
- **左键** 的行为因上下文而异

---

## 四、View 模式（相机平移）

### 4.1 行为

| 操作 | 结果 |
|:-----|:------|
| 左键拖拽 | 平移相机（沿 Right/Forward 平面移动） |
| 右键拖拽 | 旋转相机（Orbit，与当前相同） |
| 滚轮 | 缩放 |
| WASD | 移动相机 |
| 鼠标中键拖拽 | 平移相机（与左键相同，备选） |

### 4.2 实现

左键拖拽平移相机复用 `Pan` 动作（`EditorViewportInputActions.h` 中已有定义但未使用）。

在推送回调中，`CameraSystem` 注册 `Pan` 动作的回调：

```cpp
// EditorCameraSystem 注册时声明
SystemBuilder("EditorCameraSystem", PreUpdate, Render)
    .AlwaysRun()
    .WithInputDeclarations({
        { "Pan", { MouseButton(Left) }, WhileHeld,
          [this](const ActionState& s) {
              // 左键拖拽 → 平移相机
              auto panInput = s.GetAxis2D();
              float panSpeed = m_panSpeed * deltaTime;
              Strafe(-panInput.X * panSpeed);
              Walk(panInput.Y * panSpeed);
          }},
        { "OrbitCamera", { MouseButton(Right) }, WhileHeld,
          [this](const ActionState& s) {
              // 右键拖拽 → 旋转相机
              auto lookInput = s.GetAxis2D();
              Pitch(XMConvertToRadians(0.25f * lookInput.Y));
              RotateY(XMConvertToRadians(0.25f * lookInput.X));
          }},
        // ... 其他动作
    })
    .Build();
```

---

## 五、Select 模式（射线检测选中）

### 5.1 流程

```
帧 N
  ├─ 用户左键点击实体
  ├─ InputMgr::Update() 中推送 SelectClick 回调
  │     └─ 存储拾取请求（鼠标位置）到 PendingPickRequest  ← Immediate 阶段
  │
  ├─ ExecutePhase(Update)
  │     └─ SelectApplySystem 读取 PendingPickRequest       ← Update Phase
  │           ├─ 执行精确射线检测（Worker 线程）
  │           └─ 设置选中实体，广播 SelectionChanged
  │
  └─ FrameSync

帧 N+1
  └─ 选中实体高亮显示
```

### 5.2 依赖的现有设施

| 组件 | 状态 | 说明 |
|:-----|:------|:------|
| 射线检测系统 | 🚧 被注释 | 场景系统迁移时被注释，需恢复 |
| 可见集 | ✅ 可用 | 视锥剔除后的可见集可作为候选集 |
| ImGuizmo | ✅ 可用 | 已集成，修复了 API 版本兼容问题 |
| 选中实体高亮 | ❌ 未实现 | 需开发 |

### 5.3 选中状态管理

选中状态由 `Editor` 持有，通过消息广播：

```cpp
// Editor 持有
ECS::Entity m_selectedEntity = ECS::Entity::Null();

// 选中后广播消息
m_dispatcher->PostMessage(MessageIds::SelectionChanged, m_selectedEntity);
```

监听 `SelectionChanged` 消息的系统：
- `PropertiesPanel` — 更新属性卡显示
- `EditorViewportRenderSystem` — 更新高亮/轮廓渲染
- `ImGuizmo` — 更新 Gizmo 绑定的目标实体

---

## 六、编辑器 System 拆分

### 6.1 当前结构

```
EditorViewportInput（单体类，硬编码所有输入处理）
  ├─ HandleCameraInput()       ← WASD + Orbit + Zoom
  ├─ FocusOnEntity()           ← F 聚焦
  ├─ Pitch() / RotateY() / ... ← 龙书相机操作
  └─ 被 Editor::ImmediateCallback 调用
```

### 6.2 改造后结构

```
EditorViewportInput 移除，拆分为三个独立的 System：

EditorCameraSystem（Immediate 回调，推送接收）
  ├─ Move 动作 → Walk/Strafe
  ├─ Look 动作 → Pitch/RotateY
  ├─ OrbitCamera 动作 → 启用轨道
  ├─ Zoom 动作 → 调整速度
  ├─ Pan 动作 → 平移相机
  └─ FocusSelection 动作 → FocusOnEntity

EditorSelectSystem（Immediate 回调 + Update Phase System）
  ├─ [Immediate] SelectClick 动作 → 存 PendingPickRequest
  └─ [Update]   读取 PendingPickRequest → 精确检测 → 设置选中

EditorGizmoSystem（Immediate 回调 + Update Phase System）
  ├─ [Immediate] 读取 ImGuizmo 状态
  └─ [Update]    ImGuizmo delta → 写 TransformComponent

EditorToolbarSystem（Immediate 回调）
  ├─ ToolCursor 动作 → 切换到 Cursor 工具
  ├─ ToolTranslate 动作 → 切换到 Translate 工具
  ├─ ToolRotate 动作 → 切换到 Rotate 工具
  └─ 更新 ImGui 工具栏 UI
```

### 6.3 文件结构

```
Editor/EditorLib/
  ├─ Core/
  │   └── Editor.cpp/h              ← 移除 m_viewportInput，改为注册各 System
  ├─ Viewport/
  │   ├── EditorViewport.h/.cpp     ← 视口渲染管理
  │   ├── EditorViewportToolbar.h/.cpp  ← 新增：工具栏 UI + 工具模式状态
  │   ├── Systems/
  │   │   ├── EditorCameraSystem.h/.cpp   ← 新增：相机控制
  │   │   ├── EditorSelectSystem.h/.cpp   ← 新增：选中实体
  │   │   ├── EditorGizmoSystem.h/.cpp    ← 新增：Gizmo 操作
  │   │   └── EditorToolbarSystem.h/.cpp  ← 新增：工具栏 + 快捷键
  │   ├── EditorViewportInput.h/.cpp ← 移除或大幅精简
  │   └── EditorViewportInputActions.h ← 修改：新增工具切换动作
  └── Config/
      └── editor_strings_*.json       ← 修改：新增工具栏字符串
```

---

## 七、工具栏 UI

### 6.1 布局

```
┌─────────────────────────────────────────────────┐
│ 视口渲染区域                                      │
│                                                   │
│  ┌──────────────┐                                │
│  │ [🖱] [✚] [🔄] │  ← 工具栏，叠加在视口左上角      │
│  └──────────────┘                                │
│                                                   │
│  ┌──────────────────────┐                         │
│  │ 场景渲染内容          │                         │
│  │                      │                         │
│  │  ┌─┐                 │                         │
│  │  │G│ ← Gizmo          │                         │
│  │  └─┘                 │                         │
│  └──────────────────────┘                         │
└─────────────────────────────────────────────────┘
```

### 6.2 实现

在 `EditorViewport::RenderViewport()` 或 `EditorLayout::DrawViewport()` 中，使用 `ImGui::Begin/End` 在视口图像上叠加工具栏：

```cpp
void DrawViewportToolbar() {
    ImGuiIO& io = ImGui::GetIO();
    ImVec2 viewportPos = ...;  // 视口图像的位置
    ImVec2 viewportSize = ...; // 视口图像的尺寸

    // 工具栏位置：视口左上角 + 偏移
    ImGui::SetNextWindowPos(ImVec2(viewportPos.x + 8, viewportPos.y + 8));
    ImGui::SetNextWindowBgAlpha(0.6f);
    ImGui::Begin("ViewportToolbar", nullptr,
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar |
        ImGuiWindowFlags_AlwaysAutoResize);

    // 工具按钮组
    ImGui::BeginGroup();
    DrawToolButton("🖱", ViewportTool::Cursor, "Cursor (Q)");
    ImGui::SameLine();
    DrawToolButton("✚", ViewportTool::Translate, "Translate (W)");
    ImGui::SameLine();
    DrawToolButton("🔄", ViewportTool::Rotate, "Rotate (E)");
    ImGui::EndGroup();

    // 如果当前工具是 Cursor，显示子模式选择
    if (m_currentTool == ViewportTool::Cursor) {
        ImGui::SameLine();
        ImGui::SeparatorEx(ImGuiSeparatorFlags_Vertical);
        ImGui::SameLine();
        if (ImGui::Selectable("View", m_cursorMode == CursorMode::View)) {
            m_cursorMode = CursorMode::View;
        }
        ImGui::SameLine();
        if (ImGui::Selectable("Select", m_cursorMode == CursorMode::Select)) {
            m_cursorMode = CursorMode::Select;
        }
    }

    ImGui::End();
}
```

### 6.3 语言包

工具栏按钮需要支持多语言，在 `EditorStrings` 中添加：

```json
{
    "ViewportToolbar.Cursor": "Cursor",
    "ViewportToolbar.Translate": "Translate",
    "ViewportToolbar.Rotate": "Rotate",
    "ViewportToolbar.CursorMode.View": "View",
    "ViewportToolbar.CursorMode.Select": "Select"
}
```

---

## 七、快捷键

| 快捷键 | 行为 |
|:-------|:------|
| `Q` | 切换到 Cursor 工具 |
| `W` | 切换到 Translate 工具 |
| `E` | 切换到 Rotate 工具 |
| `Ctrl+Z` | Undo（后续） |
| `Ctrl+Shift+Z` | Redo（后续） |
| `Delete` | 删除选中实体（后续） |
| `F` | 聚焦到选中实体（已有） |

快捷键通过 `EditorViewportInputActions.h` 中已有的 `FocusSelection` 动作扩展，新增 `ToolCursor`、`ToolTranslate`、`ToolRotate` 动作：

```cpp
DEFINE_ACTION(ToolCursor);
DEFINE_ACTION(ToolTranslate);
DEFINE_ACTION(ToolRotate);
```

---

## 八、文件结构变更

```
Editor/EditorLib/
  ├─ Core/
  │   └── Editor.cpp                  ← 修改：持有 EditorViewportToolbar
  │   └── Editor.h
  ├─ Viewport/
  │   ├── EditorViewport.h/.cpp       ← 修改：工具栏叠加绘制
  │   ├── EditorViewportToolbar.h     ← 新增：工具栏 UI + 工具模式状态
  │   ├── EditorViewportToolbar.cpp
  │   ├── EditorViewportInput.h/.cpp  ← 修改：根据工具模式路由输入
  │   └── EditorViewportInputActions.h ← 修改：新增工具切换动作
  ├─ Scene/
  │   └── EditorViewport.cpp          ← 修改：Gizmo 集成（已存文档）
  └─ Config/
      └── editor_strings_*.json       ← 修改：新增工具栏字符串
```

---

## 九、实施步骤

### 阶段 1：工具栏基础设施

| 步骤 | 内容 | 依赖 |
|:----:|:-----|:------|
| 1.1 | 创建 `ViewportTool` / `CursorMode` 枚举 | 无 |
| 1.2 | 创建 `EditorViewportToolbar` 类（UI + 状态管理） | 1.1 |
| 1.3 | 在 `EditorViewport` 中叠加绘制工具栏 | 1.2 |
| 1.4 | 新增 `ToolCursor` / `ToolTranslate` / `ToolRotate` 动作定义 | 无 |
| 1.5 | 快捷键切换工具模式 | 1.1, 1.4 |
| 1.6 | 更新语言包 | 1.2 |

### 阶段 2：Cursor-View 模式

| 步骤 | 内容 | 依赖 |
|:----:|:-----|:------|
| 2.1 | `EditorViewportInput` 根据工具模式路由输入 | 1.1 |
| 2.2 | View 模式下左键拖拽平移相机 | 2.1 |
| 2.3 | 验证所有工具模式下右键拖拽旋转 + 滚轮缩放 + WASD 不受影响 | 2.1 |

### 阶段 3：Cursor-Select 模式 + Gizmo

| 步骤 | 内容 | 依赖 |
|:----:|:-----|:------|
| 3.1 | 恢复射线检测系统（场景系统迁移时被注释的部分） | 无 |
| 3.2 | Select 模式下左键点击触发射线检测 → 选中实体 | 3.1 |
| 3.3 | 选中实体高亮渲染 | 3.2 |
| 3.4 | Translate 工具：ImGuizmo 显示 Gizmo 并回写 Position | 1.1 |
| 3.5 | Rotate 工具：ImGuizmo 显示 Gizmo 并回写 Rotation | 1.1 |
| 3.6 | 切换工具时自动切换 ImGuizmo 操作模式 | 3.4, 3.5 |

### 阶段 4：增强（后续）

| 步骤 | 内容 | 依赖 |
|:----:|:-----|:------|
| 4.1 | Undo/Redo（操作前快照） | 3.4 |
| 4.2 | Scale 工具 | 3.4 |
| 4.3 | 多选 + 矩形选择 | 3.2 |
| 4.4 | 吸附/网格对齐 | 3.4 |

---

## 十、与现有系统的关系

### 10.1 输入系统

当前 `EditorViewportInput` 的 `Update()` 直接调用 `HandleCameraInput()`。改造后：

```cpp
void EditorViewportInput::Update(float deltaTime) {
    if (!m_initialized || !m_context)
        return;

    // 根据当前工具模式路由输入
    switch (m_currentTool) {
    case ViewportTool::Cursor:
        if (m_cursorMode == CursorMode::View)
            HandleCursorViewMode(deltaTime);
        else
            HandleCursorSelectMode(deltaTime);
        break;
    case ViewportTool::Translate:
    case ViewportTool::Rotate:
        // Gizmo 模式下，左键由 ImGuizmo 处理
        // 右键/滚轮/WASD 仍然可用
        HandleGizmoMode(deltaTime);
        break;
    }
}
```

### 10.2 ImGuizmo

ImGuizmo 的 `Manipulate()` 调用在 `EditorViewport` 的渲染 System 中执行。工具模式决定 `ImGuizmo::OPERATION`：

```cpp
// 在渲染 System 中
ImGuizmo::OPERATION gizmoOp;
switch (m_toolbar->GetCurrentTool()) {
case ViewportTool::Translate: gizmoOp = ImGuizmo::TRANSLATE; break;
case ViewportTool::Rotate:    gizmoOp = ImGuizmo::ROTATE;    break;
default:                      gizmoOp = ImGuizmo::TRANSLATE; break;
}

if (m_selectedEntity.IsValid()) {
    ImGuizmo::Manipulate(viewMatrix, projMatrix, gizmoOp, ImGuizmo::WORLD,
                         &transform.position.x);
}
```

### 10.3 选中实体

选中实体由 `Editor` 持有，`EditorViewportToolbar` / `EditorViewportInput` / `EditorViewport` 共享同一份选中状态：

```cpp
// Editor 持有选中实体，注入到各子系统
m_viewportInput->SetSelectedEntity(m_selectedEntity);
m_toolbar->SetSelectedEntity(m_selectedEntity);
// 渲染 System 从 EditorViewport 获取
```

---

## 十一、参考实现

- Cocos Creator 3.x 编辑器：视口左上角工具栏（平移/旋转/缩放/矩形选择）
- Unity Editor：Scene View 工具栏（Hand/Translate/Rotate/Scale/Rect）
- Unreal Editor：Viewport 左上角工具栏（Select/Translate/Rotate/Scale）

---

## 十二、SelectionService — 选中实体服务

> 2026-07-23 补充：选中实体是连接 Outliner、Properties、Viewport、Gizmo 的"胶水"，应独立为服务，不依赖 SceneManager。

### 12.1 设计动机

| 问题 | 说明 |
|:-----|:------|
| **选中实体归属不清晰** | 当前散落在 `EditorLayout::m_selectedEntity`、`OutlinerPanel`、`EditorViewportToolbar` 等多处 |
| **与 SceneManager 耦合** | 选中实体不一定来自场景（可能来自预览系统、调试系统） |
| **无统一通知机制** | Properties、Viewport、Gizmo 各自轮询或独立更新 |

### 12.2 设计

```cpp
// Editor/EditorLib/Core/EditorSelection.h

/// 选中实体服务 — 全局唯一选中状态，不依赖 SceneManager
class EditorSelection {
public:
    static EditorSelection &Get();

    /// 设置选中实体，广播 OnSelectionChanged
    void SetSelected(ECS::Entity entity);

    /// 获取当前选中实体
    ECS::Entity GetSelected() const { return m_selected; }

    /// 注册选中变化回调
    using CallbackId = uint32_t;
    CallbackId RegisterCallback(std::function<void(ECS::Entity)> callback);
    void UnregisterCallback(CallbackId id);

private:
    ECS::Entity m_selected = ECS::INVALID_ENTITY;
    // 回调表
    std::vector<CallbackEntry> m_callbacks;
};
```

### 12.3 使用示例

```cpp
// OutlinerPanel 点击实体
void OutlinerPanel::Draw(float) {
    if (ImGui::Selectable("EntityName", isSelected)) {
        EditorSelection::Get().SetSelected(entity);
    }
}

// Properties 监听选中变化
EditorSelection::Get().RegisterCallback(
    [this](ECS::Entity entity) {
        m_currentEntity = entity;
        // 刷新属性卡显示
    });

// GizmoSystem 监听选中变化
EditorSelection::Get().RegisterCallback(
    [this](ECS::Entity entity) {
        m_gizmoTarget = entity;
        // 更新 Gizmo 绑定的变换矩阵
    });
```

### 12.4 不依赖 SceneManager

选中实体可以来自任何来源，不限于场景：

```
选中实体来源
  ├─ OutlinerPanel → 点击场景实体
  ├─ Viewport → 射线检测选中实体（场景或预览）
  ├─ PreviewSystem → 点击预览实体
  └─ DebugSystem → 点击调试可视化实体

EditorSelection 不关心来源，只存储 Entity 引用
  └─ Properties → 读取实体组件 → 绘制属性卡
  └─ GizmoSystem → 读取 TransformComponent → 绘制 Gizmo
```

### 12.5 与 SceneManager 的关系

```cpp
// SceneManager 不负责选中实体
// 选中实体由 EditorSelection 独立管理

// 错误做法：
// sceneMgr->SetSelectedEntity(entity);  // ❌

// 正确做法：
EditorSelection::Get().SetSelected(entity);  // ✅
// 任何模块都可以选中任何实体，不关心实体来源
```