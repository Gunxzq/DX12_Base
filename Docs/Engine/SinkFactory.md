# SinkFactory 
核心目标是将“配置数据”转换为“spdlog sink 实例”，同时屏蔽底层实现细节。
1. 抽象创建逻辑：调用者（Logger）只关心传入配置，不关心 std::make_shared<...> 的具体模板参数或依赖项。
2. 隔离依赖：防止 Logger.cpp 直接 include UI 层（如 DebugOverlay）或文件系统特定头文件。
3. 统一错误处理：在创建 Sink 失败时（如文件路径无效、权限不足），统一捕获异常并返回空指针或默认 Sink，保证日志系统初始化不会导致引擎崩溃。
4. 支持扩展：未来新增 Sink（如网络日志、ETW）只需在工厂中增加一个分支，无需修改 Logger 核心代码。