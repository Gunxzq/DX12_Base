#include "EditorConsoleSink.h"

namespace DX12Engine::Logger {

void editor_console_sink_mt::sink_it_(const spdlog::details::log_msg &msg) {
    // 格式化消息
    spdlog::memory_buf_t formatted;
    spdlog::sinks::base_sink<std::mutex>::formatter_->format(msg, formatted);

    std::string formatted_text(formatted.begin(), formatted.end());

    // 线程安全地追加到环形缓冲区
    {
        std::lock_guard<std::mutex> lock(m_entryMutex);
        m_entries.emplace_back(ConsoleLogEntry{
            static_cast<int>(msg.level),
            std::string(msg.payload.data(), msg.payload.size()),
            std::move(formatted_text)
        });

        // 限制缓冲区大小
        while (m_entries.size() > m_maxEntries) {
            m_entries.pop_front();
        }
    }
}

std::vector<ConsoleLogEntry> editor_console_sink_mt::ConsumeEntries() {
    std::lock_guard<std::mutex> lock(m_entryMutex);
    if (m_entries.empty()) {
        return {};
    }

    std::vector<ConsoleLogEntry> result;
    result.reserve(m_entries.size());
    result.insert(result.end(),
        std::make_move_iterator(m_entries.begin()),
        std::make_move_iterator(m_entries.end()));
    m_entries.clear();
    return result;
}

} // namespace DX12Engine::Logger