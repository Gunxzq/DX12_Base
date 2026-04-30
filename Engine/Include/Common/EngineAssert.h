// ========== EngineAssert.h ==========
#pragma once
#include "WindowsPlatform.h"

/**
 * @brief 引擎内部错误报告宏
 *
 * 功能：
 * 1. 将错误消息输出到 VS Output 窗口 (OutputDebugStringA)
 * 2. 将错误消息输出到 stderr (控制台/日志文件)
 * 3. 在 Debug 模式下触发断点 (__debugbreak)，方便开发者立即检查堆栈
 *
 * @param msg 错误消息字符串
 */
#ifdef _DEBUG
#define ENGINE_ASSERT_MSG(msg)                                                                                         \
    do {                                                                                                               \
        ::OutputDebugStringA("[ENGINE ASSERT] ");                                                                      \
        ::OutputDebugStringA(msg);                                                                                     \
        ::OutputDebugStringA("\n");                                                                                    \
        fprintf(stderr, "[ENGINE ASSERT] %s\n", msg);                                                                  \
        __debugbreak();                                                                                                \
    } while (0)
#else
#define ENGINE_ASSERT_MSG(msg)                                                                                         \
    do {                                                                                                               \
        fprintf(stderr, "[ENGINE ASSERT] %s\n", msg);                                                                  \
    } while (0)
#endif

/**
 * @brief 格式化错误报告宏 (支持 printf 风格格式)
 * 用法: ENGINE_ASSERT_FMT("Failed to load config: %s", path.c_str());
 */
#ifdef _DEBUG
#define ENGINE_ASSERT_FMT(fmt, ...)                                                                                   \
    do {                                                                                                               \
        char assertBuf[1024];                                                                                          \
        snprintf(assertBuf, sizeof(assertBuf), fmt, __VA_ARGS__);                                                      \
        ::OutputDebugStringA("[ENGINE ASSERT] ");                                                                      \
        ::OutputDebugStringA(assertBuf);                                                                               \
        ::OutputDebugStringA("\n");                                                                                    \
        fprintf(stderr, "[ENGINE ASSERT] %s\n", assertBuf);                                                            \
        __debugbreak();                                                                                                \
    } while (0)
#else
#define ENGINE_ASSERT_FMT(fmt, ...)                                                                                   \
    do {                                                                                                               \
        fprintf(stderr, "[ENGINE ASSERT] " fmt "\n", __VA_ARGS__);                                                     \
    } while (0)
#endif

/**
 * @brief 简化的断言宏（仅 Debug 模式生效）
 */
#ifdef _DEBUG
#define ENGINE_ASSERT(condition)                                                                                       \
    do {                                                                                                               \
        if (!(condition)) {                                                                                             \
            ENGINE_ASSERT_MSG("Assertion failed: " #condition);                                                         \
        }                                                                                                              \
    } while (0)
#else
#define ENGINE_ASSERT(condition) ((void)0)
#endif
