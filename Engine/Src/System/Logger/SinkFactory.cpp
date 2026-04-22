#include "System/Logger/SinkFactory.h"
#include "Core/Config/LoggerConfig.h"

#include "System/Logger/Skinks/DebugOutputSink.h"
#include "System/Logger/Skinks/LogWindowSink.h"
#include "System/Logger/Skinks/NullSink.h"

#include "System/Logger/DebugOverlay.h"

#include <filesystem>
#include <spdlog/sinks/rotating_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>

namespace DX12Engine {
namespace Core {

std::vector<std::shared_ptr<spdlog::sinks::sink>> SinkFactory::CreateSinks(const LogConfig &config) {
    std::vector<std::shared_ptr<spdlog::sinks::sink>> sinks;

    // 1. Console
    if (config.Sinks.Console.Enabled) {
        auto sink = CreateConsoleSink(config);
        if (sink)
            sinks.push_back(sink);
    }

    // 2. File
    if (config.Sinks.File.Enabled) {
        auto sink = CreateFileSink(config);
        if (sink)
            sinks.push_back(sink);
    }

    // 3. Debug Output
    if (config.Sinks.DebugOutput.Enabled) {
        auto sink = CreateDebugOutputSink(config);
        if (sink)
            sinks.push_back(sink);
    }

    // 4. Log Window (UI Dependent)
    if (config.Sinks.LogWindow.Enabled) {
        auto sink = CreateLogWindowSink(config);
        if (sink)
            sinks.push_back(sink);
    }

    // ========================================================================
    // 保底逻辑：如果没有启用任何 Sink，或者所有 Sink 创建失败
    // 返回一个 Null Sink，确保 Logger 能够正常初始化而不崩溃
    // ========================================================================

    if (sinks.empty()) {
        fprintf(stderr, "[SinkFactory] Warning: No valid sinks created. Using Null Sink.\n");
        sinks.push_back(std::make_shared<null_sink_mt>());
    }

    return sinks;
}

std::shared_ptr<spdlog::sinks::sink> SinkFactory::CreateConsoleSink(const LogConfig &config) {
    try {
        auto sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
        sink->set_level(static_cast<spdlog::level::level_enum>(static_cast<int>(config.Sinks.Console.Level)));
        return sink;
    } catch (const std::exception &e) {
        // 使用简单的 stderr 输出错误，因为此时 logger 可能还没完全 ready
        fprintf(stderr, "[SinkFactory] Failed to create Console Sink: %s\n", e.what());
        return nullptr;
    }
}

std::shared_ptr<spdlog::sinks::sink> SinkFactory::CreateFileSink(const LogConfig &config) {
    try {
        std::filesystem::path exePath = std::filesystem::current_path();
        std::filesystem::path absoluteLogPath = config.Sinks.File.Path;

        if (absoluteLogPath.is_relative()) {
            absoluteLogPath = exePath / config.Sinks.File.Path;
        }

        // 确保目录存在
        if (auto parent = absoluteLogPath.parent_path(); !parent.empty()) {
            std::error_code ec;
            std::filesystem::create_directories(parent, ec);
        }

        auto maxSize = static_cast<size_t>(config.Sinks.File.Rotation.MaxSizeMb) * 1024 * 1024;
        auto maxFiles = static_cast<size_t>(config.Sinks.File.Rotation.MaxFiles);

        auto sink = std::make_shared<spdlog::sinks::rotating_file_sink_mt>(absoluteLogPath.string(), maxSize, maxFiles);

        sink->set_level(static_cast<spdlog::level::level_enum>(static_cast<int>(config.Sinks.File.Level)));
        return sink;
    } catch (const std::exception &e) {
        fprintf(stderr, "[SinkFactory] Failed to create File Sink: %s\n", e.what());
        return nullptr;
    }
}

std::shared_ptr<spdlog::sinks::sink> SinkFactory::CreateDebugOutputSink(const LogConfig &config) {
    try {
        // 假设你已经将 debug_output_sink 抽离到独立文件
        auto sink = std::make_shared<debug_output_sink_mt>();
        sink->set_level(static_cast<spdlog::level::level_enum>(static_cast<int>(config.Sinks.DebugOutput.Level)));
        return sink;
    } catch (const std::exception &e) {
        fprintf(stderr, "[SinkFactory] Failed to create DebugOutput Sink: %s\n", e.what());
        return nullptr;
    }
}

std::shared_ptr<spdlog::sinks::sink> SinkFactory::CreateLogWindowSink(const LogConfig &config) {
    try {
// 关键点：这里 include DebugOverlay.h，但 Logger.cpp 不需要 include
#include "System/Logger/DebugOverlay.h"

        // 显示窗口
        if (auto overlay = DebugOverlay::GetInstance()) {
            overlay->Show();
        }

        // 创建带回调的 Sink
        auto callback = [](int level, const char *payload, const std::string &text) {
            if (auto overlay = DebugOverlay::GetInstance()) {
                overlay->PushLog(static_cast<LogEntry::Level>(level), payload, text);
            }
        };

        auto sink = std::make_shared<log_window_sink_mt>(callback);
        sink->set_level(static_cast<spdlog::level::level_enum>(static_cast<int>(config.Sinks.LogWindow.Level)));
        return sink;
    } catch (const std::exception &e) {
        fprintf(stderr, "[SinkFactory] Failed to create LogWindow Sink: %s\n", e.what());
        return nullptr;
    }
}

} // namespace Core
} // namespace DX12Engine