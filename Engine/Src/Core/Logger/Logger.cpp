#include "Core/Logger/Logger.h"
#include <filesystem>
#include <iostream>
#include <shared_mutex>
#include <spdlog/async.h>
#include <spdlog/details/null_mutex.h>
#include <spdlog/sinks/rotating_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

namespace DX12Engine {
namespace Core {

template <typename Mutex> class null_sink_mt : public spdlog::sinks::base_sink<Mutex> {
protected:
    void sink_it_(const spdlog::details::log_msg &) override {
        // 什么都不做，丢弃日志消息
    }

    void flush_() override {
        // 什么都不做
    }
};

std::shared_ptr<spdlog::logger> Logger::s_logger = nullptr;

void Logger::Init(const LogConfig &config) {
    std::unique_lock<std::shared_mutex> lock(s_mutex);
    Init_Internal(config);
}
void Logger::Shutdown() {
    std::unique_lock<std::shared_mutex> lock(s_mutex);
    Shutdown_Internal();
}

std::shared_ptr<spdlog::logger> Logger::GetInstance() {
    std::shared_lock<std::shared_mutex> lock(s_mutex);
    return GetInstance_Internal();
}

// Private: REQUIRES: Caller must hold the lock (exclusive).
void Logger::Init_Internal(const LogConfig &config) {
    // 如果已经初始化，可以选择重置或忽略，这里假设允许重新初始化
    if (s_logger) {
        Shutdown_Internal();
    }

    // 1. 创建日志目录 (IO操作，建议在锁外进行，但依赖config，此处简化保留在锁内或可提取config路径后释放锁)
    // 为了严格遵循“临界区越短越好”，可以将目录创建移至锁外，但需要确保线程安全地读取config。
    // 鉴于 Init 通常只在启动时调用一次，且 config 是传入值，此处保持简单结构，若需极致优化可提取路径。
    if (config.Sinks.File.Enabled) {
        std::filesystem::path logPath(config.Sinks.File.Path);
        if (auto parent = logPath.parent_path(); !parent.empty()) {
            try {
                std::filesystem::create_directories(parent);
            } catch (const std::filesystem::filesystem_error &e) {
                std::cerr << "[Logger Init Error] Failed to create log directory '" << parent.string()
                          << "': " << e.what() << std::endl;
            }
        }
    }

    std::vector<spdlog::sink_ptr> sinks;

    // 2. 创建控制台 Sink
    if (config.Sinks.Console.Enabled) {
        try {
            auto consoleSink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
            consoleSink->set_level(
                static_cast<spdlog::level::level_enum>(static_cast<int>(config.Sinks.Console.Level)));
            sinks.push_back(consoleSink);
        } catch (const spdlog::spdlog_ex &ex) {
            std::cerr << "[Logger Init Error] Console sink initialization failed: " << ex.what() << std::endl;
        }
    }

    // 3. 创建文件 Sink
    if (config.Sinks.File.Enabled) {
        try {
            // 计算最大字节数
            auto maxSize = static_cast<size_t>(config.Sinks.File.Rotation.MaxSizeMb) * 1024 * 1024;
            auto maxFiles = static_cast<size_t>(config.Sinks.File.Rotation.MaxFiles);

            auto fileSink =
                std::make_shared<spdlog::sinks::rotating_file_sink_mt>(config.Sinks.File.Path, maxSize, maxFiles);
            fileSink->set_level(static_cast<spdlog::level::level_enum>(static_cast<int>(config.Sinks.File.Level)));
            sinks.push_back(fileSink);
        } catch (const spdlog::spdlog_ex &ex) {
            std::cerr << "[Logger Init Error] File sink initialization failed ('" << config.Sinks.File.Path
                      << "'): " << ex.what() << std::endl;
        }
    }

    if (sinks.empty()) {
        auto nullSink = std::make_shared<null_sink_mt<std::mutex>>();
        sinks.push_back(nullSink);
    }

    // 4. 构建 Async Logger
    if (config.Sinks.Async.Enabled) {
        spdlog::init_thread_pool(config.Sinks.Async.QueueSize, 1);

        spdlog::async_overflow_policy policy = spdlog::async_overflow_policy::overrun_oldest;
        if (config.Sinks.Async.OverflowPolicy == "block") {
            policy = spdlog::async_overflow_policy::block;
        }

        s_logger = std::make_shared<spdlog::async_logger>("engine_logger", sinks.begin(), sinks.end(),
                                                          spdlog::thread_pool(), policy);
    } else {
        // 同步 Logger
        s_logger = std::make_shared<spdlog::logger>("engine_logger", sinks.begin(), sinks.end());
    }

    // 5. 注册与设置默认
    spdlog::register_logger(s_logger);
    spdlog::set_default_logger(s_logger);

    // 6. 设置全局级别
    s_logger->set_level(static_cast<spdlog::level::level_enum>(static_cast<int>(config.GlobalLevel)));

    // 7. 设置刷盘级别
    s_logger->flush_on(static_cast<spdlog::level::level_enum>(static_cast<int>(config.FlushLevel)));

    // 8. 设置格式
    s_logger->set_pattern(config.FormatPattern);
}

// Private: REQUIRES: Caller must hold the lock (exclusive).
void Logger::Shutdown_Internal() {
    if (s_logger) {
        s_logger->flush();
        spdlog::shutdown();
        s_logger.reset();
    }
}

// Private: REQUIRES: Caller must hold the lock (shared).
std::shared_ptr<spdlog::logger> Logger::GetInstance_Internal() { return s_logger; }

} // namespace Core
} // namespace DX12Engine