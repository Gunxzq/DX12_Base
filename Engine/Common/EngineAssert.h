// ========== EngineAssert.h ==========
#pragma once
#include "Core/ErrorReporter.h"
#include "WindowsPlatform.h"

/**
 * @brief 条件断言宏（仅 Debug 生效）
 *
 * 用法: ASSERT(ptr != nullptr, "Unexpected null in renderer");
 *
 * Release 下完全消除，无开销。
 */
#ifdef _DEBUG
#define ASSERT(condition, fmt, ...)                                                                                    \
    do {                                                                                                               \
        if (!(condition)) {                                                                                             \
            DX12Engine::ErrorReporter::Fatal("Assertion failed: " fmt, ##__VA_ARGS__);                                  \
        }                                                                                                              \
    } while (0)
#else
#define ASSERT(condition, fmt, ...) ((void)0)
#endif
