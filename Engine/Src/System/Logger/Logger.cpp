#include "System/Logger/Logger.h"
#include "System/Logger/DebugOverlay.h"

#include <spdlog/async.h>
#include <spdlog/details/null_mutex.h>
#include <spdlog/sinks/rotating_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

namespace DX12Engine {
namespace Core {

// ========================================================================
// 辅助函数
// ========================================================================

inline void DebugOutput(const std::string &msg) { ::OutputDebugStringA(msg.c_str()); }

// ========================================================================
// Log Window Sink - 输出到独立日志窗口
// ========================================================================

template <typename Mutex> class log_window_sink_mt : public spdlog::sinks::base_sink<Mutex> {
protected:
    void sink_it_(const spdlog::details::log_msg &msg) override {
        spdlog::memory_buf_t formatted;
        base_sink<Mutex>::formatter_->format(msg, formatted);
        std::string text(formatted.begin(), formatted.end());

        // 使用 DebugOverlay 单例
        DebugOverlay *overlay = DebugOverlay::GetInstance();
        if (overlay) {
            // 映射 spdlog level 到 LogEntry level
            LogEntry::Level level = LogEntry::Info;
            switch (msg.level) {
            case spdlog::level::trace:
                level = LogEntry::Trace;
                break;
            case spdlog::level::debug:
                level = LogEntry::Debug;
                break;
            case spdlog::level::info:
                level = LogEntry::Info;
                break;
            case spdlog::level::warn:
                level = LogEntry::Warn;
                break;
            case spdlog::level::err:
                level = LogEntry::Error;
                break;
            case spdlog::level::critical:
                level = LogEntry::Critical;
                break;
            default:
                level = LogEntry::Info;
                break;
            }

            overlay->PushLog(level, msg.payload.data(), text);
        }
    }

    void flush_() override {}
};

using log_window_sink = log_window_sink_mt<std::mutex>;

// ========================================================================
// Debug Output Sink - 输出到 VS 输出窗口
// ========================================================================

template <typename Mutex> class debug_output_sink_mt : public spdlog::sinks::base_sink<Mutex> {
protected:
    void sink_it_(const spdlog::details::log_msg &msg) override {
        spdlog::memory_buf_t formatted;
        base_sink<Mutex>::formatter_->format(msg, formatted);
        formatted.push_back('\0');
        ::OutputDebugStringA(formatted.data());
    }

    void flush_() override {}
};

using debug_output_sink = debug_output_sink_mt<std::mutex>;

// ========================================================================
// Null Sink
// ========================================================================

template <typename Mutex> class null_sink_mt : public spdlog::sinks::base_sink<Mutex> {
protected:
    void sink_it_(const spdlog::details::log_msg &) override {}
    void flush_() override {}
};

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
        delete s_instance;
        s_instance = nullptr;
    }
}

void Logger::Init_Internal(const LogConfig &config) {
    if (m_logger) {
        Shutdown_Internal();
    }

    // 日志窗口
    if (config.Sinks.LogWindow.Enabled) {
        // 获取单例并显示窗口
        DebugOverlay::GetInstance()->Show();
        DebugOutput("[Logger] Log window created/shown\n");
    }

    std::vector<spdlog::sink_ptr> sinks;

    // 日志窗口 sink
    if (config.Sinks.LogWindow.Enabled) {
        try {
            auto windowSink = std::make_shared<log_window_sink>();
            windowSink->set_level(
                static_cast<spdlog::level::level_enum>(static_cast<int>(config.Sinks.LogWindow.Level)));
            sinks.push_back(windowSink);
            DebugOutput("[Logger] LogWindow sink created\n");
        } catch (const spdlog::spdlog_ex &ex) {
            DebugOutput("[Logger Init Error] LogWindow sink failed: " + std::string(ex.what()) + "\n");
        }
    }

    // 3. 创建 Debug Output Sink
    if (config.Sinks.DebugOutput.Enabled) {
        try {
            auto debugSink = std::make_shared<debug_output_sink>();
            debugSink->set_level(
                static_cast<spdlog::level::level_enum>(static_cast<int>(config.Sinks.DebugOutput.Level)));
            sinks.push_back(debugSink);
            DebugOutput("[Logger] DebugOutput sink created\n");
        } catch (const spdlog::spdlog_ex &ex) {
            DebugOutput("[Logger Init Error] DebugOutput sink failed: " + std::string(ex.what()) + "\n");
        }
    }

    // 4. 创建控制台 Sink
    if (config.Sinks.Console.Enabled) {
        try {
            auto consoleSink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
            consoleSink->set_level(
                static_cast<spdlog::level::level_enum>(static_cast<int>(config.Sinks.Console.Level)));
            sinks.push_back(consoleSink);
            DebugOutput("[Logger] Console sink created\n");
        } catch (const spdlog::spdlog_ex &ex) {
            DebugOutput("[Logger Init Error] Console sink failed: " + std::string(ex.what()) + "\n");
        }
    }

    // 5. 创建文件 Sink
    if (config.Sinks.File.Enabled) {
        try {
            std::filesystem::path exePath = std::filesystem::current_path();

            // 解析绝对路径：如果配置是相对路径，则相对于 exe 目录
            std::filesystem::path absoluteLogPath = config.Sinks.File.Path;
            if (absoluteLogPath.is_relative()) {
                absoluteLogPath = exePath / config.Sinks.File.Path;
            }

            // 确保父目录存在
            if (auto parent = absoluteLogPath.parent_path(); !parent.empty()) {
                std::error_code ec;
                std::filesystem::create_directories(parent, ec);
                if (ec) {
                    DebugOutput("[Logger Init Warning] Failed to create directory: " + ec.message() + "\n");
                }
            }

            auto maxSize = static_cast<size_t>(config.Sinks.File.Rotation.MaxSizeMb) * 1024 * 1024;
            auto maxFiles = static_cast<size_t>(config.Sinks.File.Rotation.MaxFiles);

            auto fileSink =
                std::make_shared<spdlog::sinks::rotating_file_sink_mt>(absoluteLogPath.string(), maxSize, maxFiles);
            fileSink->set_level(static_cast<spdlog::level::level_enum>(static_cast<int>(config.Sinks.File.Level)));
            sinks.push_back(fileSink);
            DebugOutput("[Logger] File sink created: " + absoluteLogPath.string() + "\n");
        } catch (const spdlog::spdlog_ex &ex) {
            DebugOutput("[Logger Init Error] File sink failed: " + std::string(ex.what()) + "\n");
        }
    }

    if (sinks.empty()) {
        auto nullSink = std::make_shared<null_sink_mt<std::mutex>>();
        sinks.push_back(nullSink);
        DebugOutput("[Logger] No sinks enabled, using null sink\n");
    }

    // 6. 构建 Async Logger
    if (config.Sinks.Async.Enabled) {
        spdlog::init_thread_pool(config.Sinks.Async.QueueSize, 1);

        spdlog::async_overflow_policy policy = spdlog::async_overflow_policy::overrun_oldest;
        if (config.Sinks.Async.OverflowPolicy == "block") {
            policy = spdlog::async_overflow_policy::block;
        }

        m_logger = std::make_shared<spdlog::async_logger>("engine_logger", sinks.begin(), sinks.end(),
                                                          spdlog::thread_pool(), policy);
    } else {
        m_logger = std::make_shared<spdlog::logger>("engine_logger", sinks.begin(), sinks.end());
    }

    // 7. 注册与设置默认
    spdlog::register_logger(m_logger);
    spdlog::set_default_logger(m_logger);

    // 8. 设置全局级别
    m_logger->set_level(static_cast<spdlog::level::level_enum>(static_cast<int>(config.GlobalLevel)));

    // 9. 设置刷盘级别
    m_logger->flush_on(static_cast<spdlog::level::level_enum>(static_cast<int>(config.FlushLevel)));

    // 10. 设置格式
    m_logger->set_pattern(config.FormatPattern);

    // 11. 测试日志
    m_logger->info("Logger initialized successfully");
    m_logger->flush();
}

void Logger::Shutdown_Internal() {
    if (m_logger) {
        m_logger->flush();
        spdlog::shutdown();
        m_logger.reset();
    }
}

} // namespace Core
} // namespace DX12Engine
