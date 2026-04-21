#pragma once

#include <memory>
#include <string>
#include <windows.h>

namespace DX12Engine {
namespace Core {

// ========================================================================
// LogWindow - Win32 日志窗口
//
// 独立的控制台风格窗口，用于显示 DebugOverlay 中的日志
// ========================================================================

class LogWindow {
public:
    LogWindow();
    ~LogWindow();

    // 禁止拷贝
    LogWindow(const LogWindow &) = delete;
    LogWindow &operator=(const LogWindow &) = delete;

    /**
     * @brief 显示窗口
     */
    void Show();

    /**
     * @brief 隐藏窗口
     */
    void Hide();

    /**
     * @brief 切换显示/隐藏
     */
    void Toggle();

    /**
     * @brief 添加日志行
     */
    void AppendLog(const std::string &text);

    /**
     * @brief 清空日志
     */
    void Clear();

    /**
     * @brief 获取窗口句柄
     */
    HWND GetHWND() const { return m_hWnd; }

    /**
     * @brief 获取实例
     */
    static LogWindow *GetInstance();

    // 窗口过程
    static LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

private:
    void RegisterWindowClass();
    void CreateWindowHandle();
    void ResizeControls();
    void UpdateScrollBar();
    void EnsureVisible();

    HWND m_hWnd = nullptr;
    HWND m_editWnd = nullptr;
    std::wstring m_className = L"DX12EngineLogWindow";

    static LogWindow *s_instance;
};

} // namespace Core
} // namespace DX12Engine
