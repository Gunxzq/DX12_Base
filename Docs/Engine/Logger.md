# Logger

日志系统由 `ConfigManager` 在引擎启动早期初始化，读取 `Config/logging_config.json` 进行配置。

### 1.1 定位与职责
- **定位**: 引擎的黑匣子和听诊器。不仅是调试工具，更是运行时监控、错误追溯和用户支持的核心基础设施。
- **上游依赖**: `ConfigManager`（获取初始化配置）。
- **下游服务**: 所有子系统（Renderer, Audio, Input, Scripting 等）。
- **核心职责**:
    - **多通道输出 (Multi-Sink Output)**: 同时向控制台、文件、内存统计模块发送日志。
    - **性能无感 (Performance Transparency)**: 通过异步线程处理 I/O，确保日志记录对主渲染线程帧率的影响降至最低（< 0.1ms/frame）。
    - **结构化诊断 (Structured Diagnostics)**: 提供带上下文（线程ID、时间戳、源码位置）的标准化日志格式。
    - **调试增强 (Debug Enhancement)**: 集成断言（Assert）机制，在严重错误时自动中断程序以便调试器介入。
    - **运行时监控 (Runtime Monitoring)**: 统计关键错误数量，支持将日志流转发至游戏内 UI 控制台。

## 2. 架构设计

### 2.1 封装层策略 (Facade Pattern)
我们不直接在业务代码中调用 `spdlog::info(...)`，而是通过 `Engine::Logger` 封装层。
- **解耦**: 未来若更换日志库，只需修改封装层。
- **扩展**: 在封装层注入“钩子（Hooks）”，实现 UI 转发、错误统计等功能。

### 2.2 核心组件与 Sinks (输出端)

- **Logger Instance**: 单例模式管理的 `std::shared_ptr<spdlog::logger>`，通常命名为 "engine_logger"。
- **Sinks 设计**:
    | Sink 类型 | 用途 | 配置策略 |
    | :--- | :--- | :--- |
    | **Console Sink** | 开发者实时反馈 | Debug/Dev: 启用，带颜色高亮。<br>Release: 禁用（节省控制台资源）。 |
    | **File Sink** | 持久化存档 | 全模式: 启用。使用轮转策略（Rotation），防止磁盘爆满。 |
    | **UI Callback Sink** | 游戏内覆盖层 | Dev: 启用。将日志推送到 ImGui 控制台窗口。 |
    | **Stats Sink** | 内部统计 | 全模式: 启用。仅计数，不输出文本，用于健康检查。 |

### 2.3 异步模型
- **线程池**: 使用 `spdlog::thread_pool`，默认配置 1 个后台线程专门处理日志写入。
- **队列策略**:
    - **Debug**: `block`（阻塞）。确保每条日志都落盘，便于复现 Bug。
    - **Release**: `overrun_oldest`（丢弃旧日志）。当队列满时，丢弃最早的日志，保证最新日志可见且不阻塞游戏线程。

### 2.4 初始化流程
1. **加载配置**: `ConfigManager` 解析 `logging_config.json`。
2. **创建 Sinks**: 根据配置创建 `stdout_color_sink_mt` 和 `rotating_file_sink_mt` 等。
3. **构建 Async Logger**: 使用 `spdlog::create_async_nb` 或 `create_async` 创建异步 logger。
4. **设置格式与级别**: 应用 `format_pattern` 和 `global_level`。
5. **注册默认 Logger**: 设置为 spdlog 的默认 logger (`spdlog::set_default_logger`)。

## 3. 配置规范

### 3.1 日志路径
- **相对路径**: `./logs/engine.log`
- **解释**: 路径相对于可执行文件（.exe）所在目录。
- **自动创建**: 初始化时若 `logs` 文件夹不存在，系统应自动创建。

### 3.2 日志级别矩阵

| 构建模式 | 全局级别 (Global Level) | 控制台级别 | 文件级别 | 说明 |
| :--- | :--- | :--- | :--- | :--- |
| **Debug** | Trace | Trace | Trace | 极致详细，包含 DX12 资源绑定细节。 |
| **Development** | Info | Info | Debug | 常规开发，文件保留更多细节以备查。 |
| **Release** | Warn | Off | Warn | 仅记录警告和错误，性能优先。 |

### 3.3 格式控制

**标准格式 Pattern**:

| 占位符 | 说明 |
| :--- | :--- |
| `%Y-%m-%d %H:%M:%S.%e` | 高精度时间戳 |
| `%^%l%$` | 日志级别（带颜色标记，仅在支持颜色的 Sink 生效） |
| `%t` | 线程 ID（对于排查多线程竞态条件至关重要） |
| `%s:%#` | 源文件名和行号（注意：获取行号有轻微性能开销，Release 版可考虑移除此项） |
| `%v` | 实际日志内容 |

## 4. 配置映射 (JSON -> C++)

`Config/logging_config.json` 中的字段与 spdlog 配置的对应关系如下：

| JSON 字段 | 说明 | spdlog 对应操作 |
| :--- | :--- | :--- |
| `global_level` | 全局最低日志级别 | `logger->set_level(spdlog::level::...)` |
| `flush_level` | 触发立即刷盘的级别 | `logger->flush_on(spdlog::level::...)` |
| `format_pattern` | 日志输出格式 | `spdlog::set_pattern(...)` |
| `sinks.console.enabled` | 是否启用控制台输出 | 条件创建 `stdout_color_sink_mt` |
| `sinks.console.colorize` | 是否启用颜色 | `stdout_color_sink_mt` 默认支持 |
| `sinks.file.path` | 日志文件路径 | `rotating_file_sink_mt` 构造函数参数 |
| `sinks.file.rotation.max_size_mb` | 单文件最大大小 | `rotating_file_sink_mt` 参数 `max_size` |
| `sinks.file.rotation.max_files` | 最大保留文件数 | `rotating_file_sink_mt` 参数 `max_files` |
| `sinks.async.queue_size` | 异步队列大小 | `spdlog::init_thread_pool(queue_size, 1)` |
| `sinks.async.overflow_policy` | 队列满策略 | `spdlog::async_overflow_policy::block` 或 `overrun_oldest` |

## 5. 高级功能实现

### 5.1 断言宏 (LOG_ASSERT)

结合日志与调试中断，快速定位 DX12 COM 错误或逻辑违例。

```cpp
// Engine/Include/Core/Logger.h
#ifdef _DEBUG
    #define LOG_ASSERT(condition, msg) \
        do { \
            if (!(condition)) { \
                SPDLOG_CRITICAL("ASSERTION FAILED: {} at {}:{}", msg, __FILE__, __LINE__); \
                __debugbreak(); /* 触发断点 */ \
            } \
        } while (0)
#else
    #define LOG_ASSERT(condition, msg) ((void)0) // Release 版完全消除开销
#endif
```

用法:

```cpp
HRESULT hr = device->CreateCommittedResource(...);
LOG_ASSERT(SUCCEEDED(hr), "Failed to create vertex buffer");
```

### 5.2 错误统计与健康检查

在 Logger 封装层维护原子计数器：

```cpp
std::atomic<int> error_count_{0};

void LogError(const std::string& msg) {
    error_count_.fetch_add(1);
    spdlog::error(msg);
}

// 引擎退出时调用
void CheckHealth() {
    if (error_count_ > 100) {
        MessageBox(NULL, "Session ended with excessive errors. Check logs.", "Warning", MB_OK);
    }
}

```

### 5.3 UI 对接 (ImGui Console)

通过注册回调函数，将日志实时推送到游戏内控制台：

```cpp
// 在 Logger 初始化时注册
spdlog::logger::sink_ptr ui_sink = std::make_shared<ImGuiLogSink>();
logger->sinks().push_back(ui_sink);

// ImGuiLogSink 内部实现
void ImGuiLogSink::sink_it_(const spdlog::details::log_msg& msg) {
    spdlog::memory_buf_t formatted;
    formatter_->format(msg, formatted);
    // 将 formatted.data() 添加到 ImGui 的环形缓冲区
    ImGuiConsole::AddLog(fmt::to_string(formatted).c_str());
}
```

## 6. CMake 依赖与编译定义

在 `CMakeLists.txt` 中，我们需要确保 spdlog 正确链接，并根据构建类型添加预处理宏以优化性能。

```cpp
find_package(spdlog CONFIG REQUIRED)
target_link_libraries(${PROJECT_NAME} PRIVATE spdlog::spdlog)

# 可选：在 Release 模式下禁用 spdlog 的行号获取以提升性能
target_compile_definitions(${PROJECT_NAME} PRIVATE 
    $<$<CONFIG:Release>:SPDLOG_NO_SOURCE_LOCATION>
)
```

## 7. 最佳实践

- 避免频繁日志: 不要在每帧更新的循环中打印 Info 或 Debug 日志。如有必要，使用“节流”策略（每 N 秒打印一次）。
- 敏感信息脱敏: Release 版日志中严禁打印用户隐私数据或密钥。
- 异步队列溢出: 如果在控制台看到 "Async log queue overflow"，说明日志产生速度远超写入速度。此时应提高日志级别或增大 `queue_size。`
- DX12 调试层联动: 当 DX12 Debug Layer 报告错误时，应通过 `LOG_ERROR` 捕获并重定向到我们的日志系统，以便统一查看。

## 8. 配置示例参考

详见 logging_config.json。该文件由 `ConfigManager` 加载，并传递给 `Logger::Init()`。

```json
{
  "logging": {
    "global_level": "debug",
    "flush_level": "err",
    "format_pattern": "[%Y-%m-%d %H:%M:%S.%e] [%l] [thread %t] %v",
    
    "sinks": {
      "console": {
        "enabled": true,
        "level": "info",
        "colorize": true
      },
      "file": {
        "enabled": true,
        "level": "debug",
        "path": "logs/engine.log",
        "rotation": {
          "policy": "size",
          "max_size_mb": 10,
          "max_files": 5
        }
      },
      "async": {
        "enabled": true,
        "queue_size": 8192,
        "overflow_policy": "discard"
      }
    }
  }
}
```