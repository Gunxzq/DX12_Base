#pragma once

#include <spdlog/sinks/sink.h>

// 前置声明配置结构，避免包含大头文件
namespace DX12Engine {

namespace Boot {
struct LogConfig;      // 假设在 LoggerConfig.h 中定义
struct SinkConfigBase; // 如果配置有基类
} // namespace Boot

namespace Logger {

// ========================================================================
// SinkFactory - 静态工厂类
// 负责根据配置创建具体的 spdlog sink 实例
// ========================================================================

class SinkFactory {
public:
    // 禁止实例化
    SinkFactory() = delete;
    ~SinkFactory() = delete;

    /**
     * @brief 创建所有启用的 Sink
     * @param config 全局日志配置
     * @return 包含所有已创建 sink 的向量
     * @note 如果某个 Sink 创建失败，会记录错误并跳过，不会中断整体初始化
     */
    static std::vector<std::shared_ptr<spdlog::sinks::sink>> CreateSinks(const Boot::LogConfig &config);

private:
    // 内部辅助方法：创建单个类型的 Sink

    static std::shared_ptr<spdlog::sinks::sink> CreateConsoleSink(const Boot::LogConfig &config);
    static std::shared_ptr<spdlog::sinks::sink> CreateFileSink(const Boot::LogConfig &config);
    static std::shared_ptr<spdlog::sinks::sink> CreateDebugOutputSink(const Boot::LogConfig &config);
};

} // namespace Logger
} // namespace DX12Engine