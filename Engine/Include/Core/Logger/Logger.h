#pragma once
#include "Core/Logger/LoggerConfig.h"
#include <memory>
#include <shared_mutex>
#include <string>

namespace spdlog {
class logger;
}

namespace DX12Engine {
namespace Core {

class Logger {
public:
    /**
     * @brief 初始化日志系统
     * @param config 日志配置结构体
     * @attention Thread-safe: Acquires exclusive lock.
     */
    static void Init(const LogConfig &config);

    /**
     * @brief 关闭日志系统
     * @attention Thread-safe: Acquires exclusive lock.
     */
    static void Shutdown();

    /**
     * @brief 获取单例 Logger 实例
     * @return 单例 Logger 实例
     * @attention Thread-safe: Acquires shared lock.
     */
    static std::shared_ptr<spdlog::logger> GetInstance();

    // 便捷日志宏或静态方法可以在这里声明
    // 例如: static void Log(LogLevel level, const char* fmt, ...);

private:
    /**
     * @brief 内部初始化日志系统
     * @attention REQUIRES: Caller must hold the lock (exclusive).
     * @param config 日志配置结构体
     */
    static void Init_Internal(const LogConfig &config);

    /**
     * @brief 内部关闭日志系统
     * @attention REQUIRES: Caller must hold the lock (exclusive).
     */
    static void Shutdown_Internal();

    /**
     * @brief 获取单例 Logger 实例
     * @attention REQUIRES: Caller must hold the lock (shared).
     * @return 单例 Logger 实例
     */
    static std::shared_ptr<spdlog::logger> GetInstance_Internal();

    static std::shared_ptr<spdlog::logger> s_logger;

    inline static std::shared_mutex s_mutex;
};

} // namespace Core
} // namespace DX12Engine