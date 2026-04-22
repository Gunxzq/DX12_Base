#pragma once
#include "Common/Common.h"
#include <spdlog/sinks/base_sink.h>

namespace DX12Engine {
namespace Core {

// 定义回调函数类型
// 参数：level (int), payload (原始消息), formatted_text (格式化后的完整日志行)
using LogWindowCallback = std::function<void(int level, const char *payload, const std::string &formatted_text)>;

/**
 * @brief Log Window Sink 模板实现
 * 通过回调函数将日志发送到外部处理器（如 DebugOverlay）
 *
 * @tparam Mutex 互斥锁类型
 */
template <typename Mutex> class log_window_sink_impl : public spdlog::sinks::base_sink<Mutex> {
public:
    /**
     * @brief 构造函数
     * @param callback 当日志产生时调用的回调函数
     */
    explicit log_window_sink_impl(LogWindowCallback callback);

protected:
    void sink_it_(const spdlog::details::log_msg &msg) override;
    void flush_() override {}

private:
    LogWindowCallback m_callback;
};

// ========================================================================
// 类型别名定义
// ========================================================================

using log_window_sink_mt = log_window_sink_impl<std::mutex>;
using log_window_sink_st = log_window_sink_impl<spdlog::details::null_mutex>;

} // namespace Core
} // namespace DX12Engine