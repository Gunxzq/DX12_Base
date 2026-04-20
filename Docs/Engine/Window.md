# Window (窗口类)

## 定位和职责

### 定位

- **上游依赖**：依赖 `ConfigManager` 的配置参数来注册窗口，依赖 `Logger` 记录日志信息。
- **下游服务**：为 `Renderer` 提供 `HWND` 句柄和窗口尺寸，为 `InputSystem` 提供原始窗口消息。

### 职责

Window 类是**操作系统（Win32）**与**游戏引擎（Bootstrap/DX12）**之间的"适配器"。它不是 UI 框架，而是一个"系统句柄与事件的管理容器"。

| 职责领域 | ✅ 必须做 | ❌ 绝不涉及 |
|---------|---------|-----------|
| 生命周期 | 负责 `CreateWindow` 和 `DestroyWindow` | 不负责管理 DX12 设备或游戏逻辑的生命周期 |
| 消息处理 | 拦截 `WM_CLOSE`（退出）、`WM_SIZE`（尺寸变化）、`WM_KEYDOWN`（基础退出键） | 不负责复杂的 UI 交互（如按钮点击、菜单弹出），那是 GUI 库的事 |
| 数据提供 | 提供 `HWND`（给 DX12 用）、`Width/Height`（给渲染管线用） | 不负责存储渲染资源（如 Texture 或 Buffer） |
| 输入处理 | 仅提供原始消息或极简的按键状态查询 | 不做复杂的输入映射（如"W键=前进"），这属于 `InputSystem` |

## 架构设计

### 所有权与依赖关系

- **Bootstrap 拥有所有权**：`Window` 对象由 `Bootstrap` 创建和销毁。
- **配置注入**：`Window` 不应该自己去 `new ConfigManager`。在构造函数中接收 `WindowDesc`（配置数据的纯结构体）。
- **DX12 的被动性**：DX12 Device 只读取 `Window` 的 `HWND` 和尺寸。`Window` 不应该知道 DX12 Device 的存在（即 `Window.h` 中不应该 `#include <d3d12.h>`）。

## 功能模块

### A. 描述符模块 (Descriptor)

用于从配置中读取数据并传递给窗口。

- **数据**：标题、初始宽、初始高、是否全屏、是否可见。

```cpp
struct WindowDesc {
    std::string Title = "DX12 Engine";
    uint32_t Width = 1280;
    uint32_t Height = 720;
    bool Fullscreen = false;
    bool Visible = true;
};
```

### B. 核心窗口模块 (Core Window)

负责 Win32 API 的调用。

- **注册**：`RegisterClassEx`
- **创建**：`CreateWindowEx`
- **句柄管理**：存储 `m_hWnd`

### C. 消息代理模块 (Message Proxy)

这是"接管"的核心。需要一个机制将 Windows 的全局回调"路由"到类的成员函数中。

- **静态 `WndProc`**：Windows 系统调用的入口。
- **路由逻辑**：利用 `SetWindowLongPtr` 将 `this` 指针与 `HWND` 绑定，从而调用成员函数 `HandleMessage`。
- **事件翻译**：
  - `WM_CLOSE` → 设置 `m_ShouldClose = true`
  - `WM_SIZE` → 更新 `m_Width`, `m_Height`

### D. 轮询模块 (Polling)

游戏循环不需要阻塞式的消息泵。

- **功能**：`ProcessMessages()`
- **实现**：使用 `PeekMessage` 而非 `GetMessage`。如果没有消息，立即返回，不阻塞渲染线程。

```cpp
void Window::ProcessMessages() {
    MSG msg;
    while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
}
```
