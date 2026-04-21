# DebugOverlay

## 概述

独立日志窗口系统，用于显示引擎运行时的日志信息。

## 架构

```text
Logger
  └── LogWindowSink (线程安全的 sink)
        │
        ▼
LogWindow (Win32 Edit 控件)
  └── 直接显示日志内容
```

> **注意**: 当前实现无独立消息队列，日志通过 Win32 SendMessage 直接写入 UI 控件。spdlog 的 `log_window_sink_mt` 自带 mutex 保护，无需额外队列。

## 核心组件

### LogWindow
- **类型**: 单例模式
- **功能**: 独立的 Win32 窗口，显示日志内容
- **实现**:
  - 注册窗口类 `DX12EngineLogWindow`
  - 创建多行只读编辑框
  - 支持 UTF-8 编码显示

### DebugOverlay
- **类型**: 单例模式
- **功能**: 日志窗口管理（当前为空实现，暂存后续扩展）

## 生命周期

1. **创建**: `Bootstrap::InitializeModules()` 中初始化 Logger 时创建 LogWindow
2. **使用**: Logger 通过 `LogWindowSink` 将日志输出到 LogWindow
3. **销毁**: `Bootstrap::Shutdown()` 中销毁 LogWindow

## 集成方式

通过 `LogWindowSink` 集成到 Logger 的 sink 链中：

```cpp
// Logger.cpp
if (config.Sinks.LogWindow.Enabled) {
    auto windowSink = std::make_shared<log_window_sink>();
    sinks.push_back(windowSink);
}
```

## 配置

在 `Config/logging_config.json` 中启用：

```json
{
  "Sinks": {
    "LogWindow": {
      "Enabled": true,
      "Level": "trace"
    }
  }
}
```
