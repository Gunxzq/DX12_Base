#pragma once

#include <cstdarg>
#include <cstdio>
#include <cstdlib>

namespace DX12Engine {
namespace Logger {
class Logger;
} // namespace Logger

// ========================================================================
// ErrorReporter — 统一错误报告器
//
// 职责：
//   - 接收模块的错误调用，统一路由到 OutputDebugString + Logger + 弹窗
//   - Fatal: 致命错误，记录日志 + Messagebox + abort()
//   - Report: 一般错误，记录日志 + OutputDebugString
//   - 日志未就绪时仅走 OutputDebugString，不崩溃
// ========================================================================

class ErrorReporter {
public:
    static void SetLogger(Logger::Logger *logger);

    [[noreturn]] static void Fatal(const char *format, ...);

    static void Report(const char *format, ...);

private:
    static void VFatal(const char *format, va_list args);
    static void VReport(const char *format, va_list args);

    static Logger::Logger *s_logger;
    static char s_buffer[2048];
};

} // namespace DX12Engine
