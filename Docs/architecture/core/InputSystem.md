# 输入系统架构

> 日期：2026-07-23

---

## 一、整体架构

```
┌──────────────┐    ┌──────────────────┐    ┌────────────────────┐
│  RawInput    │───→│  InputSystem     │───→│  回调 (Callbacks)  │
│  Buffer      │    │                  │    │                    │
│              │    │  EvaluateActions │    │  InvokeCallbacks() │
│  键盘/鼠标/  │    │  → 更新状态      │    │  → 按 Trigger-     │
│  手柄原始数据│    │  PerformEdge     │    │    Behavior 过滤   │
│              │    │  → 边缘检测      │    │  → 推送回调        │
└──────────────┘    └──────────────────┘    └────────────────────┘
                          │
                          ↓
                    ┌──────────────────┐
                    │  InputActionState │
                    │  (每 Action 一个)  │
                    │                    │
                    │  Digital / Analog1D│
                    │  Axis2D / Toggle   │
                    │  Charge / ...      │
                    └──────────────────┘
```

## 二、三层数据模型

### 第一层：原始输入类型（`EInputActionType`）

定义在 `Engine/Platform/Input/Core/InputActionType.h`，描述硬件信号的**解释方式**：

```cpp
enum class EInputActionType {
    Digital,     // 数字信号：Pressed/Released/Held
    Analog1D,    // 一维模拟：-1.0~1.0（如油门、滚轮）
    Axis2D,      // 二维轴：WASD、右摇杆
    Chord,       // 组合键：Shift+W
    Tap,         // 短按：快速按下并释放
    Hold,        // 长按/蓄力：按下后随时间增加
    Toggle,      // 开关切换：每次触发翻转状态
    DoubleTap,   // 双击：窗口时间内两次按下
    HoldRelease, // 长按释放：按住后释放时触发
    Repeat,      // 重复触发：按住时按间隔重复
    Sequence     // 按键序列
};
```

### 第二层：运行时状态（`InputActionState`）

定义在 `Engine/Platform/Input/Core/InputActionState.h`，每个 Action 一个实例，存储**当前帧的完整状态**：

```cpp
struct InputActionState {
    // 数字状态（8种边缘检测结果）
    bool bPressed, bReleased, bHeld;           // 基础三态
    bool bTapped, bDoubleTapped;               // 短按/双击
    bool bLongPressed, bHoldRelease;           // 长按/长按释放
    bool bRepeatTrigger;                       // 重复触发

    // 值类型（联合体，按 EActionValueType 区分）
    union {
        float Value1D;                         // Analog1D: -1.0~1.0
        struct { float X, Y; } Axis2D;         // Axis2D: (-1.0~1.0, -1.0~1.0)
        float ChargeValue;                     // Hold 蓄力值: 0.0~1.0
    };

    bool bToggleState;                         // Toggle 当前状态
    float HoldDuration;                        // 当前已按住时间
};
```

### 第三层：触发时机（`TriggerBehavior`）

定义在 `Engine/Platform/Input/InputSystem.h`，作为**回调触发过滤器**，决定"要不要调这个回调"：

```cpp
enum class TriggerBehavior : uint8_t {
    WhileHeld,      // ← bHeld 时触发
    OnPressed,      // ← bPressed 时触发
    OnReleased,     // ← bReleased 时触发
    Axis2D,         // ← Axis2D 值变化超过阈值时触发
    OnTapped,       // ← bTapped 时触发
    OnDoubleTap,    // ← bDoubleTapped 时触发
    OnHoldRelease,  // ← bHoldRelease 时触发
    OnRepeat,       // ← bRepeatTrigger 时触发
    Analog1D,       // ← Analog1D 值变化超过阈值时触发
};
```

> **注意**：`TriggerBehavior` 只控制**是否调用回调**，回调函数参数始终传入完整的 `InputActionState`，所以回调内部可以访问所有状态（包括不在 TriggerBehavior 中的字段）。

## 三、TriggerBehavior 语义详解

### 基础触发（4种）

| TriggerBehavior | 触发条件 | 典型用途 | 每帧触发次数 |
|:----------------|:---------|:---------|:------------|
| `WhileHeld`     | `GetHeld() == true` 的每一帧 | 持续移动、蓄力 | 每帧 1 次 |
| `OnPressed`     | `GetPressed() == true` 的瞬间帧 | 跳跃、射击、切换 | 按下瞬间 1 次 |
| `OnReleased`    | `GetReleased() == true` 的瞬间帧 | 放箭、选择点击 | 释放瞬间 1 次 |
| `Axis2D`        | 二维轴值任一分量 > 阈值 | 移动 WASD、摇杆 Look | 值变化时每帧 1 次 |

### 扩展触发（5种）

| TriggerBehavior | 触发条件 | 典型用途 | 语义说明 |
|:----------------|:---------|:---------|:---------|
| `OnTapped`      | `GetTapped() == true` | 轻攻击、快速交互 | 按下后在短时间阈值（0.5s）内释放 |
| `OnDoubleTap`   | `GetDoubleTapped() == true` | 闪避、双击选择 | 两次 Tap 在双击间隔（0.3s）内 |
| `OnHoldRelease` | `GetHoldRelease() == true` | 蓄力攻击、语音输入 | 按住超过长按阈值后释放时触发 |
| `OnRepeat`      | `GetRepeatTrigger() == true` | 文本输入、连续操作 | 按住时按 RepeatInterval 间隔触发 |
| `Analog1D`      | `GetValue1D() > 阈值` | 油门、滚轮缩放 | 单轴模拟值变化时触发 |

### 触发控制能力

#### 当前实现（边缘检测，`PerformEdgeDetection`）

```
帧 N-1: 空闲        帧 N: 按下          帧 N+1: 按住         帧 N+2: 释放
                                                      
bPressed:  false  →  true (单帧)   →  false          →  false
bHeld:     false  →  true          →  true           →  false
bReleased: false  →  false         →  false          →  true (单帧)
bTapped:   false  →  false         →  false          →  true (if <0.5s)
bDoubleTap:false  →  false         →  false          →  true (if 两次间隔<0.3s)
bLongPressed:false→  false         →  true (if >0.5s)→  false
bHoldRelease:false→  false         →  false          →  true (if 曾长按)
```

#### 时序控制参数

| 参数 | 默认值 | 说明 |
|:-----|:-------|:------|
| `LONG_PRESS_THRESHOLD` | 0.5s | 长按判定阈值，超过此时间视为长按 |
| `DOUBLE_TAP_INTERVAL` | 0.3s | 双击判定窗口，两次按下间隔在此时间内视为双击 |
| `AXIS_DEADZONE` | 0.1 | 轴死区，低于此值的输入被忽略 |
| `GAMEPAD_DEADZONE` | 0.2 | 手柄死区 |

#### 回调注册方式

两种方式：

1. **手动注册**（当前主流，Editor 端使用）：
```cpp
m_callbackIds[0] = m_inputSystem->BindCallback(
    ActionId_Move,
    [this](const InputActionState &state) {
        // 处理移动输入
    },
    TriggerBehavior::Axis2D);
```

2. **声明式注册**（SystemInfo 扩展，待实现桥接）：
```cpp
SystemBuilder("EditorCameraSystem", PreUpdate, Render)
    .AlwaysRun()
    .WithInputDeclarations({
        { "Move", { Key('W'), Key('S'), Key('A'), Key('D') }, TriggerBehavior::Axis2D },
        { "Jump", { Key(Space) }, TriggerBehavior::OnPressed },
    });
```

## 四、数据流详解

### 每帧更新流程

```
InputSystem::Update()
  │
  ├─ 1. SavePreviousFrameState()
  │    保存当前帧的 bHeld 状态到 m_prevFrameActive
  │
  ├─ 2. ResetCurrentFrameState()
  │    清空当前帧的瞬时状态（bPressed/bReleased 等置 false）
  │
  ├─ 3. EvaluateActions(rawBuffer, enabledActions, currentTime)
  │    遍历启用的 Action，读取 RawInputBuffer 更新状态：
  │    ├─ 数字键 → SetDigital(pressed, released, held, ...)
  │    ├─ 单轴   → SetAnalog1D(value)
  │    └─ 双轴   → SetAxis2D(x, y)
  │
  ├─ 4. PerformEdgeDetection(currentTime)
  │    边缘检测（Pressed/Released/Tapped/DoubleTap/LongPress/HoldRelease）：
  │    ├─ held=1, prev=0 → bPressed=true, bHeld=true
  │    ├─ held=0, prev=1 → bReleased=true, 计算 Tap/DoubleTap/HoldRelease
  │    └─ held=1, prev=1 → 检测 LongPress
  │
  └─ 5. InvokeCallbacks()
       遍历 m_callbacks，按 TriggerBehavior 过滤后调用回调
```

### 状态更新规则（EvaluateActions）

```
单轴输入（仅 X 或仅 Y 非零）→ SetAnalog1D(value)
双轴输入（X 和 Y 均非零）   → SetAxis2D(x, y)
无轴输入                    → SetDigital(...)
```

## 五、与大型引擎对比

| 特性 | 本项目 | Unreal Engine | Unity Input System |
|:-----|:-------|:--------------|:-------------------|
| 触发模型 | 枚举过滤 | 状态机（Started/Ongoing/Completed/Canceled） | 状态机（Started/Performed/Canceled） |
| 触发类型数 | 9 | 6 | 5 |
| 边缘检测 | 内置（Pressed/Released/Held） | 内置（Started/Completed） | 内置（Performed） |
| 时序控制 | 长按阈值、双击间隔 | Action 配置（Hold/ Tap 等） | Interactions（Hold/Tap/MultiTap） |
| 声明式绑定 | SystemInfo.inputDeclarations（待桥接） | Input Mapping Contexts | Input Action Assets |
| 回调内完整状态 | ✅ 是（InputActionState 完整传入） | ✅ 是（FInputActionInstance） | ✅ 是（InputAction.CallbackContext） |