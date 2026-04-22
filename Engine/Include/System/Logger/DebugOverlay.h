#pragma once

#include <atomic>
#include <chrono>
#include <deque>
#include <mutex>
#include <string>
#include <windows.h>

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
// DebugOverlay - 单例，日志覆盖层/独立窗口
//
// 职责：
//   - 从 Logger 的线程安全队列中消费日志
//   - 在独立的 Win32 窗口中显示日志
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
    // 窗口管理接口
    // ========================================================================

    /**
     * @brief 创建并显示日志窗口
     */
    void Show();

    /**
     * @brief 隐藏日志窗口
     */
    void Hide();

    /**
     * @brief 销毁日志窗口
     */
    void Destroy();

    bool IsVisible() const { return m_visible && m_hwnd != nullptr; }

    // ========================================================================
    // 配置
    // ========================================================================

    void SetMaxLines(size_t maxLines) { m_maxLines = maxLines; }

private:
    DebugOverlay() = default;
    ~DebugOverlay(); // 析构函数需要实现以清理窗口

    DebugOverlay(const DebugOverlay &) = delete;
    DebugOverlay &operator=(const DebugOverlay &) = delete;

    // ========================================================================
    // 内部
    // ========================================================================

    /**
     * @brief 处理消息队列，将日志追加到窗口控件
     */
    void ProcessQueue();

    /**
     * @brief 窗口过程回调
     */
    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

    /**
     * @brief 实例特定的窗口过程处理
     */
    LRESULT HandleMessage(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

    // 线程安全的输入队列（来自 Logger）
    std::deque<LogEntry> m_incomingQueue;
    std::mutex m_queueMutex;

    // UI 状态
    HWND m_hwnd = nullptr;  // 日志窗口句柄
    HWND m_hEdit = nullptr; // 编辑框控件句柄
    bool m_visible = false;
    size_t m_maxLines = 1000; // 最大保留行数

    // 单例
    inline static DebugOverlay *s_instance = nullptr;
};

} // namespace Core
} // namespace DX12Engine