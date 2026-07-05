#include "Core/ErrorReporter.h"

#include "Logger/Logger.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace {

// UTF-8 → UTF-16 转换（供 MessageBoxW 使用）
std::wstring Utf8ToWide(const char *utf8) {
    int len = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, nullptr, 0);
    if (len <= 0) return {};
    std::wstring result(len - 1, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8, -1, &result[0], len);
    return result;
}

} // namespace

namespace DX12Engine {

Logger::Logger *ErrorReporter::s_logger = nullptr;
char ErrorReporter::s_buffer[2048] = {};

void ErrorReporter::SetLogger(Logger::Logger *logger) {
    s_logger = logger;
}

void ErrorReporter::Fatal(const char *format, ...) {
    va_list args;
    va_start(args, format);
    VFatal(format, args);
    va_end(args);
}

void ErrorReporter::Report(const char *format, ...) {
    va_list args;
    va_start(args, format);
    VReport(format, args);
    va_end(args);
}

void ErrorReporter::VFatal(const char *format, va_list args) {
    vsnprintf(s_buffer, sizeof(s_buffer), format, args);

#ifdef _DEBUG
    // OutputDebugString（仅 Debug，Release 无调试器不可见）
    OutputDebugStringA("[FATAL] ");
    OutputDebugStringA(s_buffer);
    OutputDebugStringA("\n");

    // stderr（仅 Debug，Release GUI 应用无控制台）
    fprintf(stderr, "[FATAL] %s\n", s_buffer);
#endif

    // 弹窗（Debug/Release，使用 MessageBoxW 避免 UTF-8 乱码）
    std::wstring wideMsg = Utf8ToWide(s_buffer);
    MessageBoxW(nullptr, wideMsg.c_str(), L"Fatal Error", MB_OK | MB_ICONERROR | MB_SYSTEMMODAL);

    // Logger（如已就绪，Debug/Release 都写日志文件）
    if (s_logger) {
        s_logger->Critical(s_buffer);
        s_logger->Flush();
    }

    // 强制终止（不运行静态析构，确保立即退出）
    TerminateProcess(GetCurrentProcess(), 1);
}

void ErrorReporter::VReport(const char *format, va_list args) {
    vsnprintf(s_buffer, sizeof(s_buffer), format, args);

#ifdef _DEBUG
    // OutputDebugString + stderr（仅 Debug）
    OutputDebugStringA("[ERROR] ");
    OutputDebugStringA(s_buffer);
    OutputDebugStringA("\n");
    fprintf(stderr, "[ERROR] %s\n", s_buffer);
#endif

    // Logger（Debug/Release 都写日志文件）
    if (s_logger) {
        s_logger->Error(s_buffer);
    }
}

} // namespace DX12Engine
