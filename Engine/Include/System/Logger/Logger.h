#pragma once
#include "Common/Common.h"
#include <spdlog/logger.h>

namespace DX12Engine {
namespace Core {

struct LogConfig;

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
     * @attention Thread-safe. 如果实例不存在则创建（但未初始化）。
     */
    static Logger *GetInstance();

    /**
     * @brief 初始化日志系统（必须在使用前调用）
     * @param config 日志配置结构体
     * @throw std::runtime_error 如果初始化失败（如无法创建任何 Sink 或 Logger 实例）
     * @attention Thread-safe
     */
    static void Init(const LogConfig &config);

    /**
     * @brief 关闭日志系统
     * @attention Thread-safe. 调用后 Logger 将处于未初始化状态，再次使用需重新 Init。
     */
    static void Shutdown();

    /**
     * @brief 测试专用：安全重置 Logger 状态
     * @details 与 Shutdown() 的区别：
     *         - 不调用 spdlog::shutdown()（全局一次性操作）
     *         - 只清理当前 logger 实例
     *         - 支持在测试中多次调用
     * @attention 仅供测试使用，生产代码应使用 Shutdown()
     */
    static void TestReset();

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

    /**
     * @brief 析构函数：确保资源释放
     */
    ~Logger();

    // 禁止拷贝和移动
    Logger(const Logger &) = delete;
    Logger &operator=(const Logger &) = delete;
    Logger(Logger &&) = delete;
    Logger &operator=(Logger &&) = delete;

    /**
     * @brief 内部初始化（需要持有锁）
     * @throw std::runtime_error
     */
    void Init_Internal(const LogConfig &config);

    /**
     * @brief 内部关闭（需要持有锁）
     */
    void Shutdown_Internal();

    /**
     * @brief 测试专用内部重置（需要持有锁）
     * @details 使用 spdlog::drop() 而非 spdlog::shutdown()，支持多次调用
     */
    void TestReset_Internal();

    // 底层 logger
    std::shared_ptr<spdlog::logger> m_logger;

    // 线程安全锁
    inline static std::shared_mutex s_mutex;

    // 单例实例
    inline static Logger *s_instance = nullptr;

    // 标记是否已初始化，用于安全检查
    inline static bool s_isInitialized = false;
};

} // namespace Core
} // namespace DX12Engine