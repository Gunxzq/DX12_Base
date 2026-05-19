#pragma once
#include <mutex>
#include <spdlog/sinks/base_sink.h>

namespace DX12Engine {
namespace Core {

/**
 * @brief Null Sink
 * 丢弃所有日志消息。用于测试或当没有配置其他 Sink 时的保底方案。
 *
 * @tparam Mutex 互斥锁类型
 */
template <typename Mutex> class null_sink_impl : public spdlog::sinks::base_sink<Mutex> {
protected:
    void sink_it_(const spdlog::details::log_msg &) override {
        //  intentionally empty - discard log
    }

    void flush_() override {
        // intentionally empty
    }
};

// ========================================================================
// 类型别名定义
// ========================================================================

using null_sink_mt = null_sink_impl<std::mutex>;
using null_sink_st = null_sink_impl<spdlog::details::null_mutex>;

} // namespace Core
} // namespace DX12Engine