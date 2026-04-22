#include "System/Logger/Logger.h"
#include "Core/Config/LoggerConfig.h"
#include "System/Logger/SinkFactory.h"

#include <spdlog/async.h>
#include <spdlog/details/null_mutex.h>
#include <spdlog/sinks/rotating_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

namespace DX12Engine {
namespace Core {

// ========================================================================
// Logger 单例实现
// ========================================================================

Logger *Logger::GetInstance() {
    if (!s_instance) {
        std::unique_lock<std::shared_mutex> lock(s_mutex);
        if (!s_instance) {
            s_instance = new Logger();
        }
    }
    return s_instance;
}

void Logger::Init(const LogConfig &config) {
    std::unique_lock<std::shared_mutex> lock(s_mutex);
    GetInstance()->Init_Internal(config);
}

void Logger::Shutdown() {
    std::unique_lock<std::shared_mutex> lock(s_mutex);
    if (s_instance) {
        s_instance->Shutdown_Internal();
        s_isInitialized = false;
    }
}

Logger::~Logger() {
    // 确保在静态对象销毁时清理资源
    // 由于 s_instance 是静态指针，其指向对象的析构可能在 spdlog 全局对象之后
    // 因此最好依赖显式的 Shutdown 调用
    if (m_logger) {
        try {
            Shutdown_Internal();
        } catch (...) {
            // 析构函数中不应抛出异常
        }
    }
}

void Logger::Init_Internal(const LogConfig &config) {
    // 如果已经初始化，先关闭旧的
    if (s_isInitialized && m_logger) {
        Shutdown_Internal();
    }

    // 1. 委托工厂创建所有 Sinks
    auto sinks = SinkFactory::CreateSinks(config);

    try {
        // 2. 构建 Async 或 Sync Logger
        if (config.Sinks.Async.Enabled) {
            // 初始化线程池 (如果尚未初始化)
            if (!spdlog::thread_pool()) {
                spdlog::init_thread_pool(config.Sinks.Async.QueueSize, 1);
            }

            spdlog::async_overflow_policy policy = spdlog::async_overflow_policy::overrun_oldest;
            if (config.Sinks.Async.OverflowPolicy == "block") {
                policy = spdlog::async_overflow_policy::block;
            }

            m_logger = std::make_shared<spdlog::async_logger>("engine_logger", sinks.begin(), sinks.end(),
                                                              spdlog::thread_pool(), policy);
        } else {
            m_logger = std::make_shared<spdlog::logger>("engine_logger", sinks.begin(), sinks.end());
        }

        // 3. 注册与设置默认
        spdlog::register_logger(m_logger);
        spdlog::set_default_logger(m_logger);

        // 4. 配置参数
        m_logger->set_level(static_cast<spdlog::level::level_enum>(static_cast<int>(config.GlobalLevel)));
        m_logger->flush_on(static_cast<spdlog::level::level_enum>(static_cast<int>(config.FlushLevel)));
        m_logger->set_pattern(config.FormatPattern);

        // 5. 标记为已初始化
        s_isInitialized = true;

        // 6. 测试日志
        m_logger->info("Logger initialized successfully");
        m_logger->flush();

    } catch (const std::exception &e) {
        s_isInitialized = false;
        m_logger.reset();
        throw std::runtime_error(std::string("[Logger] Initialization failed: ") + e.what());
    }
}

void Logger::Shutdown_Internal() {
    if (!m_logger) {
        return;
    }

    try {
        // 1. 刷新所有缓冲区，确保日志写入磁盘/控制台
        m_logger->flush();

        // 2. 重置本地共享指针，减少引用计数
        // 如果是 async_logger，这会触发后台线程在处理完队列后退出
        m_logger.reset();

        // 3. 关闭 spdlog 全局状态
        // 这会 join 所有后台线程，并清除注册表
        spdlog::shutdown();

    } catch (const std::exception &e) {
        // 记录到 stderr，因为 logger 已经不可用
        fprintf(stderr, "[Logger] Error during shutdown: %s\n", e.what());
    }

    s_isInitialized = false;
}

} // namespace Core
} // namespace DX12Engine
