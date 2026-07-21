#pragma once

#include <deque>
#include <mutex>
#include <string>
#include <vector>
#include <spdlog/sinks/base_sink.h>

namespace DX12Engine::Logger {

// ========================================================================
// 控制台日志条目（供 Editor ImGui Console 面板使用）
// ========================================================================

struct ConsoleLogEntry {
    int level;           // spdlog level (trace=0..critical=5)
    std::string payload; // 原始日志消息
    std::string formatted; // 格式化后的完整日志行
};

// ========================================================================
// EditorConsoleSink — 线程安全的环形缓冲区 sink
// 专门供 EditorLayout::DrawConsole 每帧消费
// ========================================================================

class editor_console_sink_mt : public spdlog::sinks::base_sink<std::mutex> {
public:
    editor_console_sink_mt() = default;

    /// 消费所有待处理的日志条目（由 EditorLayout 每帧调用）
    std::vector<ConsoleLogEntry> ConsumeEntries();

    /// 设置最大缓冲条目数（默认 2000）
    void SetMaxEntries(size_t max) { m_maxEntries = max; }

protected:
    void sink_it_(const spdlog::details::log_msg &msg) override;
    void flush_() override {}

private:
    std::mutex m_entryMutex;
    std::deque<ConsoleLogEntry> m_entries;
    size_t m_maxEntries = 2000;
};

} // namespace DX12Engine::Logger