#pragma once
#include "Common/Common.h"
#include "Event/EventTypes.h"
#include "Resource.h"
#include <string>

namespace DX12Engine {

namespace Boot {
struct WindowConfig;
}

namespace Input {
class InputManager;
}

namespace Platform {

struct WindowConfig;

class Window {
public:
    Window(const Boot::WindowConfig &config);
    ~Window();

    // 1. 初始化与创建
    bool Create();

    // 2. 消息循环 (在 Bootstrap 的主循环中调用)
    void ProcessMessages();

    // 3. 状态查询
    bool ShouldClose() const { return m_ShouldClose; }
    HWND GetHandle() const { return m_hWnd; }
    uint32_t GetWidth() const { return m_Width; }
    uint32_t GetHeight() const { return m_Height; }

    void SetCursorCapture(bool capture);
    bool IsCursorCaptured() const { return m_cursorCaptured; }

    // 显示窗口
    void Show();
    void SetFullscreen(bool fullscreen);
    bool IsFullscreen() const { return m_IsFullscreen; }

    void SetInputManager(Input::InputManager *inputMgr);

private:
    // 静态 WndProc，用于接收 Windows 消息
    static LRESULT CALLBACK WndProcStatic(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);
    // 成员 WndProc，处理具体逻辑
    LRESULT WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

    // 发送窗口大小变化事件
    void PostWindowResizeEvent(uint32_t width, uint32_t height);

private:
    HWND m_hWnd = nullptr;
    HINSTANCE m_hInstance = nullptr;

    // 存储简单状态
    std::wstring m_Title;
    uint32_t m_InitWidth = 0;
    uint32_t m_InitHeight = 0;
    bool m_IsResizable = false;

    // 运行时状态
    uint32_t m_Width = 0;
    uint32_t m_Height = 0;
    bool m_ShouldClose = false;

    bool m_IsFullscreen = false;
    RECT m_WindowedRect = {0}; // 保存窗口模式时的位置和大小

    bool m_cursorCaptured = false;

    // 是否处于模态大小调整状态（用户拖拽调整大小时为 true）
    bool m_InSizeMove = false;
    // 拖拽期间尺寸是否真正变化（用于减少 WM_EXITSIZEMOVE 时的不必要事件）
    bool m_SizeChangedDuringMove = false;

    Input::InputManager *m_inputMgr = nullptr;
};

} // namespace Platform
} // namespace DX12Engine
