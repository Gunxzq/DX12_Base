#pragma once

#include "Common/Common.h"

#include <nlohmann/json.hpp>

namespace DX12Engine {
namespace Boot {

// ========================================================================
// 日志级别定义
// ========================================================================

enum class LogLevel : int { Trace = 0, Debug = 1, Info = 2, Warn = 3, Error = 4, Critical = 5, Off = 6 };

// 辅助宏：用于 nlohmann json 序列化 enum class 为字符串
NLOHMANN_JSON_SERIALIZE_ENUM(LogLevel, {{LogLevel::Trace, "trace"},
                                        {LogLevel::Debug, "debug"},
                                        {LogLevel::Info, "info"},
                                        {LogLevel::Warn, "warn"},
                                        {LogLevel::Error, "error"},
                                        {LogLevel::Critical, "critical"},
                                        {LogLevel::Off, "off"}})

// ========================================================================
// 异步配置结构体
// ========================================================================

struct AsyncConfig {
    bool Enabled = true;
    int QueueSize = 8192;
    // overflow_policy: "block" or "discard" (mapped to spdlog::async_overflow_policy)
    std::string OverflowPolicy = "discard";

    NLOHMANN_DEFINE_TYPE_INTRUSIVE(AsyncConfig, Enabled, QueueSize, OverflowPolicy)
};

// ========================================================================
// 文件轮转配置结构体?
// ========================================================================

struct FileRotationConfig {
    std::string Policy = "size"; // 目前主要支持 size
    int MaxSizeMb = 10;
    int MaxFiles = 5;

    NLOHMANN_DEFINE_TYPE_INTRUSIVE(FileRotationConfig, Policy, MaxSizeMb, MaxFiles)
};

// ========================================================================
// 文件 Sink 配置结构体
// ========================================================================

struct FileSinkConfig {
    bool Enabled = true;
    LogLevel Level = LogLevel::Debug; // 鏂囦欢閫氬父璁板綍鏇磋缁嗙殑鏃ュ織
    std::string Path = "logs/engine.log";
    FileRotationConfig Rotation;

    NLOHMANN_DEFINE_TYPE_INTRUSIVE(FileSinkConfig, Enabled, Level, Path, Rotation)
};

// ========================================================================
// 控制台 Sink 配置结构体
// ========================================================================

struct ConsoleSinkConfig {
    bool Enabled = true;
    LogLevel Level = LogLevel::Info;
    bool Colorize = true;

    NLOHMANN_DEFINE_TYPE_INTRUSIVE(ConsoleSinkConfig, Enabled, Level, Colorize)
};

// ========================================================================
// Debug Output Sink 配置结构体 (VS 输出窗口)
// ========================================================================

struct DebugOutputSinkConfig {
    bool Enabled = false;
    LogLevel Level = LogLevel::Debug;

    NLOHMANN_DEFINE_TYPE_INTRUSIVE(DebugOutputSinkConfig, Enabled, Level)
};

// ========================================================================
// 所有 Sinks 的聚合配置
// ========================================================================

struct SinksConfig {
    ConsoleSinkConfig Console;
    FileSinkConfig File;
    DebugOutputSinkConfig DebugOutput;
    AsyncConfig Async;

    NLOHMANN_DEFINE_TYPE_INTRUSIVE(SinksConfig, Console, File, DebugOutput, Async)
};

// ========================================================================
// 顶层日志配置结构体
// ========================================================================

/**
 * @brief 完整日志配置
 * 对应 JSON 中的 "logging" 对象
 */
struct LogConfig {
    LogLevel GlobalLevel = LogLevel::Info;
    LogLevel FlushLevel = LogLevel::Error; // 对应 flush_level
    std::string FormatPattern = "[%Y-%m-%d %H:%M:%S.%e] [%l] [thread %t] %v";

    SinksConfig Sinks;

    NLOHMANN_DEFINE_TYPE_INTRUSIVE(LogConfig, GlobalLevel, FlushLevel, FormatPattern, Sinks)
};

} // namespace Boot
} // namespace DX12Engine