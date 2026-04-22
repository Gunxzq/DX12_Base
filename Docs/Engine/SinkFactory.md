# SinkFactory

## 核心目标

将**配置数据**转换为 **spdlog sink 实例**，同时屏蔽底层实现细节。

## 设计原则

| 目标 | 说明 |
|------|------|
| 抽象创建逻辑 | 调用者（Logger）只关心传入配置，不关心 `std::make_shared<...>` 的具体模板参数 |
| 隔离依赖 | 防止 Logger.cpp 直接 include UI 层（如 DebugOverlay）或文件系统特定头文件 |
| 统一错误处理 | 创建失败时统一捕获异常，返回空指针或默认 Sink，保证初始化不崩溃 |
| 支持扩展 | 新增 Sink（如网络日志、ETW）只需在工厂中增加分支，无需修改 Logger 核心代码 |

## 架构图

```
┌─────────────────────────────────────────────────────────┐
│                      Logger                              │
│                   (日志系统核心)                          │
└─────────────────────┬───────────────────────────────────┘
                      │ LogConfig
                      ▼
┌─────────────────────────────────────────────────────────┐
│                    SinkFactory                           │
│                   (静态工厂类)                           │
├─────────────────┬─────────────────┬─────────────────────┤
│ CreateConsole   │ CreateFile      │ CreateDebugOutput   │
│ Sink()          │ Sink()          │ Sink()              │
├─────────────────┼─────────────────┼─────────────────────┤
│ CreateLogWindow │ ...             │ (扩展预留)           │
│ Sink()          │                 │                     │
└─────────────────┴─────────────────┴─────────────────────┘
          │               │               │
          ▼               ▼               ▼
   ┌──────────┐    ┌────────────┐   ┌──────────────┐
   │ Console  │    │ File       │   │ DebugOutput  │
   │ (彩色)   │    │ (旋转文件)  │   │ (调试输出)   │
   └──────────┘    └────────────┘   └──────────────┘
```

## 支持的 Sink 类型

| Sink 类型 | 实现类 | 功能 |
|-----------|--------|------|
| Console | `spdlog::sinks::stdout_color_sink_mt` | 彩色控制台输出 |
| File | `spdlog::sinks::rotating_file_sink_mt` | 旋转文件日志（自动分文件） |
| DebugOutput | `debug_output_sink_mt` | Windows OutputDebugString |
| LogWindow | `log_window_sink_mt` | UI 窗口实时显示日志 |

## 关键设计

### 1. 禁止实例化

```cpp
class SinkFactory {
public:
    SinkFactory() = delete;
    ~SinkFactory() = delete;
    // ...
};
```

### 2. 前向声明避免循环依赖

```cpp
// SinkFactory.h
namespace DX12Engine {
namespace Core {

struct LogConfig;      // ✅ 前向声明，不包含大头文件

class SinkFactory {
    static std::vector<std::shared_ptr<spdlog::sinks::sink>> CreateSinks(const LogConfig &config);
};

} // namespace Core
} // namespace DX12Engine
```

### 3. 统一错误处理

每个创建方法都包含 `try-catch`，失败时返回 `nullptr`：

```cpp
static std::shared_ptr<spdlog::sinks::sink> CreateFileSink(const LogConfig &config) {
    try {
        // 创建 sink...
        return sink;
    } catch (const std::exception &e) {
        fprintf(stderr, "[SinkFactory] Failed to create File Sink: %s\n", e.what());
        return nullptr;  // 失败返回空指针
    }
}
```

### 4. 空 Sink 保底逻辑

```cpp
// 如果没有启用任何 Sink，或者所有 Sink 创建失败
if (sinks.empty()) {
    fprintf(stderr, "[SinkFactory] Warning: No valid sinks created. Using Null Sink.\n");
    sinks.push_back(std::make_shared<null_sink_mt>());  // 保底
}
```

## 使用示例

```cpp
// Logger.cpp - 不需要知道具体有哪些 Sink 类型
void Logger::Init(const LogConfig &config) {
    auto sinks = SinkFactory::CreateSinks(config);
    // 直接使用 vector<shared_ptr<sink>> 创建 logger
}
```

## 扩展方式

新增 Sink 只需三步：

1. 在 `SinkFactory.h` 添加私有方法声明
2. 在 `SinkFactory.cpp` 实现创建逻辑
3. 在 `CreateSinks()` 中添加分支调用

```cpp
// SinkFactory.h
private:
    static std::shared_ptr<spdlog::sinks::sink> CreateNetworkSink(const LogConfig &config);

// SinkFactory.cpp
static std::shared_ptr<spdlog::sinks::sink> SinkFactory::CreateNetworkSink(const LogConfig &config) {
    try {
        return std::make_shared<network_sink_mt>(config.Sinks.Network.Host);
    } catch (const std::exception &e) {
        fprintf(stderr, "[SinkFactory] Failed to create Network Sink: %s\n", e.what());
        return nullptr;
    }
}
```
