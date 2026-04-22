#pragma once
#include "Common/Common.h"
#include <spdlog/sinks/base_sink.h>

// 仅在 Windows 平台下需要包含 windows.h 用于 OutputDebugStringA
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace DX12Engine {
namespace Core {

/**
 * @brief Debug Output Sink 模板实现
 * 将日志消息发送到 Windows Debug Output (Visual Studio Output 窗口)
 *
 * @tparam Mutex 互斥锁类型
 */
template <typename Mutex> class debug_output_sink_impl : public spdlog::sinks::base_sink<Mutex> {
protected:
    /**
     * @brief 核心写入逻辑
     * @param msg spdlog 日志消息结构体
     */
    void sink_it_(const spdlog::details::log_msg &msg) override {
        // 1. 格式化消息
        spdlog::memory_buf_t formatted;
        base_sink<Mutex>::formatter_->format(msg, formatted);

        // 2. 确保以 null 结尾，因为 OutputDebugStringA 需要 C-style 字符串
        formatted.push_back('\0');

        // 3. 输出到 Debug Window
#ifdef _WIN32
        ::OutputDebugStringA(formatted.data());
#endif
    }

    /**
     * @brief 刷新逻辑
     * Debug Output 是即时输出的，通常不需要额外的 flush 操作
     */
    void flush_() override {}
};

// ========================================================================
// 类型别名定义
// ========================================================================

// 多线程安全版本 (Multi-Threaded) - 推荐使用
using debug_output_sink_mt = debug_output_sink_impl<std::mutex>;

// 单线程版本 (Single-Threaded) - 仅在确定单线程调用时使用以获得更高性能
using debug_output_sink_st = debug_output_sink_impl<spdlog::details::null_mutex>;

} // namespace Core
} // namespace DX12Engine