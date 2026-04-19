#pragma once

#include <memory>
#include <string>

#include "Core/Logger/LoggerConfig.h"

// 前置声明 spdlog 相关类型，避免在头文件中暴露 spdlog 细节
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
     */
    static void Init(const LogConfig &config);

    /**
     * @brief 关闭日志系统
     */
    static void Shutdown();

    /**
     * @brief 获取单例 Logger 实例
     */
    static std::shared_ptr<spdlog::logger> &GetInstance();

    // 便捷日志宏或静态方法可以在这里声明
    // 例如: static void Log(LogLevel level, const char* fmt, ...);

private:
    static std::shared_ptr<spdlog::logger> s_logger;
};

} // namespace Core
} // namespace DX12Engine