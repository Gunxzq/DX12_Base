#pragma once
#include "Core/Logger/LoggerConfig.h"
#include <spdlog/logger.h>
#include <memory>
#include <shared_mutex>
#include <string>

namespace DX12Engine {
namespace Core {

// ========================================================================
// Logger - 单例日志系统
// 基础设施模块，通过依赖注入提供给各系统使用
// ========================================================================

class Logger {
public:
    // ========================================================================
    // 单例访问
    // ========================================================================

    /**
     * @brief 获取 Logger 单例实例
     * @return Logger 单例指针
     * @attention Thread-safe
     */
    static Logger *GetInstance();

    /**
     * @brief 初始化日志系统（必须在使用前调用）
     * @param config 日志配置结构体
     * @attention Thread-safe
     */
    static void Init(const LogConfig &config);

    /**
     * @brief 关闭日志系统
     * @attention Thread-safe
     */
    static void Shutdown();

    // ========================================================================
    // 日志方法
    // ========================================================================

    template <typename... Args> void Trace(const char *fmt, Args... args) const {
        if (m_logger)
            m_logger->trace(fmt, args...);
    }

    template <typename... Args> void Debug(const char *fmt, Args... args) const {
        if (m_logger)
            m_logger->debug(fmt, args...);
    }

    template <typename... Args> void Info(const char *fmt, Args... args) const {
        if (m_logger)
            m_logger->info(fmt, args...);
    }

    template <typename... Args> void Warn(const char *fmt, Args... args) const {
        if (m_logger)
            m_logger->warn(fmt, args...);
    }

    template <typename... Args> void Error(const char *fmt, Args... args) const {
        if (m_logger)
            m_logger->error(fmt, args...);
    }

    template <typename... Args> void Critical(const char *fmt, Args... args) const {
        if (m_logger)
            m_logger->critical(fmt, args...);
    }

    /**
     * @brief 刷新所有 sink
     */
    void Flush() const {
        if (m_logger)
            m_logger->flush();
    }

private:
    Logger() = default;
    ~Logger() = default;

    // 禁止拷贝和移动
    Logger(const Logger &) = delete;
    Logger &operator=(const Logger &) = delete;
    Logger(Logger &&) = delete;
    Logger &operator=(Logger &&) = delete;

    /**
     * @brief 内部初始化（需要持有锁）
     */
    void Init_Internal(const LogConfig &config);

    /**
     * @brief 内部关闭（需要持有锁）
     */
    void Shutdown_Internal();

    // 底层 logger
    std::shared_ptr<spdlog::logger> m_logger;

    // 线程安全锁
    inline static std::shared_mutex s_mutex;

    // 单例实例
    inline static Logger *s_instance = nullptr;
};

} // namespace Core
} // namespace DX12Engine
