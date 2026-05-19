#include "LogWindowSink.h"

namespace DX12Engine {
namespace Core {

template <typename Mutex>
log_window_sink_impl<Mutex>::log_window_sink_impl(LogWindowCallback callback) : m_callback(std::move(callback)) {}

template <typename Mutex> void log_window_sink_impl<Mutex>::sink_it_(const spdlog::details::log_msg &msg) {
    // 如果没有注册回调，直接返回
    if (!m_callback) {
        return;
    }

    // 1. 格式化消息
    spdlog::memory_buf_t formatted;
    spdlog::sinks::base_sink<Mutex>::formatter_->format(msg, formatted);

    // 转换为 std::string 以便传递给回调
    std::string formatted_text(formatted.begin(), formatted.end());

    // 2. 调用回调
    // 注意：这里传递 msg.level (int) 和 msg.payload (原始消息) 以及格式化后的文本
    // 接收方可以根据需要决定显示什么
    m_callback(static_cast<int>(msg.level), msg.payload.data(), formatted_text);
}

// 显式实例化
template class log_window_sink_impl<std::mutex>;
// template class log_window_sink_impl<spdlog::details::null_mutex>;

} // namespace Core
} // namespace DX12Engine