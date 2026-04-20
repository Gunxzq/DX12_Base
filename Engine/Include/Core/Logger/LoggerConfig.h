#pragma once

#include <nlohmann/json.hpp>
#include <string>

namespace DX12Engine {
namespace Core {

// ========================================================================
// 鏃ュ織绾у埆瀹氫箟
// ========================================================================

enum class LogLevel : int { Trace = 0, Debug = 1, Info = 2, Warn = 3, Error = 4, Critical = 5, Off = 6 };

// 杈呭姪瀹忥細鐢ㄤ簬 nlohmann json 搴忓垪鍖?enum class 涓哄瓧绗︿覆
NLOHMANN_JSON_SERIALIZE_ENUM(LogLevel, {{LogLevel::Trace, "trace"},
                                        {LogLevel::Debug, "debug"},
                                        {LogLevel::Info, "info"},
                                        {LogLevel::Warn, "warn"},
                                        {LogLevel::Error, "error"},
                                        {LogLevel::Critical, "critical"},
                                        {LogLevel::Off, "off"}})

// ========================================================================
// 寮傛閰嶇疆缁撴瀯浣?
// ========================================================================

struct AsyncConfig {
    bool Enabled = true;
    int QueueSize = 8192;
    // overflow_policy: "block" or "discard" (mapped to spdlog::async_overflow_policy)
    std::string OverflowPolicy = "discard";

    NLOHMANN_DEFINE_TYPE_INTRUSIVE(AsyncConfig, Enabled, QueueSize, OverflowPolicy)
};

// ========================================================================
// 鏂囦欢杞浆閰嶇疆缁撴瀯浣?
// ========================================================================

struct FileRotationConfig {
    std::string Policy = "size"; // 鐩墠涓昏鏀寔 size
    int MaxSizeMb = 10;
    int MaxFiles = 5;

    NLOHMANN_DEFINE_TYPE_INTRUSIVE(FileRotationConfig, Policy, MaxSizeMb, MaxFiles)
};

// ========================================================================
// 鏂囦欢 Sink 閰嶇疆缁撴瀯浣?
// ========================================================================

struct FileSinkConfig {
    bool Enabled = true;
    LogLevel Level = LogLevel::Debug; // 鏂囦欢閫氬父璁板綍鏇磋缁嗙殑鏃ュ織
    std::string Path = "logs/engine.log";
    FileRotationConfig Rotation;

    NLOHMANN_DEFINE_TYPE_INTRUSIVE(FileSinkConfig, Enabled, Level, Path, Rotation)
};

// ========================================================================
// 鎺у埗鍙?Sink 閰嶇疆缁撴瀯浣?
// ========================================================================

struct ConsoleSinkConfig {
    bool Enabled = true;
    LogLevel Level = LogLevel::Info;
    bool Colorize = true;

    NLOHMANN_DEFINE_TYPE_INTRUSIVE(ConsoleSinkConfig, Enabled, Level, Colorize)
};

// ========================================================================
// 鎵€鏈?Sinks 鐨勮仛鍚堥厤缃?
// ========================================================================

struct SinksConfig {
    ConsoleSinkConfig Console;
    FileSinkConfig File;
    AsyncConfig Async;

    NLOHMANN_DEFINE_TYPE_INTRUSIVE(SinksConfig, Console, File, Async)
};

// ========================================================================
// 椤跺眰鏃ュ織閰嶇疆缁撴瀯浣?
// ========================================================================

/**
 * @brief 瀹屾暣鏃ュ織閰嶇疆
 * 瀵瑰簲 JSON 涓殑 "logging" 瀵硅薄
 */
struct LogConfig {
    LogLevel GlobalLevel = LogLevel::Info;
    LogLevel FlushLevel = LogLevel::Error; // 瀵瑰簲 flush_level
    std::string FormatPattern = "[%Y-%m-%d %H:%M:%S.%e] [%l] [thread %t] %v";

    SinksConfig Sinks;

    NLOHMANN_DEFINE_TYPE_INTRUSIVE(LogConfig, GlobalLevel, FlushLevel, FormatPattern, Sinks)
};

} // namespace Core
} // namespace DX12Engine