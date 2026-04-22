#pragma once

#include <atomic>
#include <chrono>
#include <deque>
#include <mutex>
#include <string>

namespace DX12Engine {
namespace Core {

// ========================================================================
// 日志条目
// ========================================================================

struct LogEntry {
    enum Level : int { Trace = 0, Debug = 1, Info = 2, Warn = 3, Error = 4, Critical = 5 };

    std::chrono::steady_clock::time_point timestamp;
    Level level;
    std::string message;
    std::string formatted; // 预格式化的字符串

    LogEntry(Level lvl, std::string msg, std::string fmt);
};

// ========================================================================
// DebugOverlay - 单例，日志覆盖层
//
// 职责：
//   - 从 Logger 的线程安全队列中消费日志
//   - 在屏幕上绘制日志窗口
// ========================================================================

class DebugOverlay {
public:
    static DebugOverlay *GetInstance();

    // ========================================================================
    // 消费者接口（Logger 调用）
    // ========================================================================

    /**
     * @brief 线程安全地添加日志条目到队列
     */
    void PushLog(LogEntry::Level level, const std::string &message, const std::string &formatted);

    // ========================================================================
    // 生产者接口（渲染系统调用）
    // ========================================================================

    /**
     * @brief 更新：消费队列中的日志到本地列表
     */
    void Update();

    /**
     * @brief 渲染：绘制日志覆盖层
     */
    void Render();

    /**
     * @brief 清空所有日志
     */
    void Clear();

    // ========================================================================
    // 配置
    // ========================================================================

    void SetMaxLines(size_t maxLines) { m_maxLines = maxLines; }
    void SetVisible(bool visible) { m_visible = visible; }
    bool IsVisible() const { return m_visible; }

    // 切换显示/隐藏
    void Toggle() { m_visible = !m_visible; }

private:
    DebugOverlay() = default;
    ~DebugOverlay() = default;

    DebugOverlay(const DebugOverlay &) = delete;
    DebugOverlay &operator=(const DebugOverlay &) = delete;

    // ========================================================================
    // 内部
    // ========================================================================

    void ProcessQueue();

    // 线程安全的输入队列（来自 Logger）
    std::deque<LogEntry> m_incomingQueue;
    std::mutex m_queueMutex;

    // 本地显示列表（只在主线程访问，不需要锁）
    std::deque<LogEntry> m_displayList;
    size_t m_maxLines = 100;

    // UI 状态
    bool m_visible = true;
    bool m_autoScroll = true;

    // 单例
    inline static DebugOverlay *s_instance = nullptr;
};

} // namespace Core
} // namespace DX12Engine
