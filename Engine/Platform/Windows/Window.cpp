#include "Window.h"
#include "Boot/WindowConfig.h"
#include "Event/MessageDispatcher.h"
#include <Windows.h>
#include <Windowsx.h>

using namespace DX12Engine::Boot;
using namespace DX12Engine::Event;

namespace DX12Engine::Platform {

Window::Window(const Boot::WindowConfig &config)
    : m_Title(config.title), m_InitWidth(config.width), m_InitHeight(config.height), m_IsResizable(config.resizable) {}

Window::~Window() {
    if (m_hWnd) {
        DestroyWindow(m_hWnd);
        m_hWnd = nullptr;
    }
}

bool Window::Create() {

    // 获取实例句柄
    m_hInstance = GetModuleHandle(nullptr);

    // 注册窗口类
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(WNDCLASSEX);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WndProcStatic; // 窗口处理函数
    wc.hInstance = m_hInstance;

    wc.hIcon = LoadIcon(m_hInstance, MAKEINTRESOURCE(IDI_DX12BASE)); // 图标
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);                     // 光标
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);                   // 背景色
    wc.lpszClassName = L"DX12WindowClass";                           // 窗口类名
    wc.hIconSm = LoadIcon(m_hInstance, MAKEINTRESOURCE(IDI_SMALL));  // 小图标

    // 注册
    if (!RegisterClassExW(&wc)) {
        return false;
    }

    // 指定样式
    DWORD style = WS_OVERLAPPEDWINDOW;
    if (!m_IsResizable) {
        style &= ~WS_THICKFRAME;
        style &= ~WS_MAXIMIZEBOX;
    }

    RECT rect = {0, 0, static_cast<LONG>(m_InitWidth), static_cast<LONG>(m_InitHeight)};
    AdjustWindowRect(&rect, style, FALSE);

    m_hWnd = CreateWindowExW(0, L"DX12WindowClass", m_Title.c_str(), style, CW_USEDEFAULT, CW_USEDEFAULT,
                             rect.right - rect.left, rect.bottom - rect.top, nullptr, nullptr, m_hInstance, this);
    if (!m_hWnd)
        return false;

    // 设置窗口位置
    SetWindowPos(m_hWnd, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);

    m_Width = m_InitWidth;
    m_Height = m_InitHeight;

    return true;
}

void Window::Show() {
    if (m_hWnd) {
        ShowWindow(m_hWnd, SW_SHOW);
        UpdateWindow(m_hWnd);
    }
}

void Window::ProcessMessages() {
    MSG msg = {};

    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
}

/**
 * @brief 窗口处理函数
 * @param hWnd 句柄
 * @param msg   消息
 * @param wParam
 * @param lParam
 * @return LRESULT
 * @date 2026-04-21
 */
LRESULT CALLBACK Window::WndProcStatic(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {

    // msg：创建窗口
    if (msg == WM_NCCREATE) {
        CREATESTRUCT *pCreate = reinterpret_cast<CREATESTRUCT *>(lParam);
        Window *pThis = reinterpret_cast<Window *>(pCreate->lpCreateParams);

        SetWindowLongPtr(hWnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(pThis));

        return DefWindowProcW(hWnd, msg, wParam, lParam);
    }

    // 从窗口用户数据区取出之前保存的 this 指针
    Window *pThis = reinterpret_cast<Window *>(GetWindowLongPtr(hWnd, GWLP_USERDATA));
    if (pThis) {
        // 处理其他消息
        return pThis->WndProcHandler(hWnd, msg, wParam, lParam);
    }

    // 默认处理
    return DefWindowProcW(hWnd, msg, wParam, lParam);
}

void Window::SetCursorCapture(bool capture) {
    if (m_cursorCaptured == capture) {
        return;
    }

    m_cursorCaptured = capture;

    if (capture) {

        m_rawInputBuffer.Reset();

        SetCapture(m_hWnd);

        // 2. 隐藏系统光标
        ShowCursor(FALSE);

        // 3. 将光标移动到窗口中心，防止初始视角跳变
        RECT rect;
        GetClientRect(m_hWnd, &rect);
        POINT center;
        center.x = (rect.left + rect.right) / 2;
        center.y = (rect.top + rect.bottom) / 2;
        ClientToScreen(m_hWnd, &center);
        SetCursorPos(center.x, center.y);

        m_rawInputBuffer.Reset();
    } else {
        // 1. 释放捕获
        ReleaseCapture();

        // 2. 显示系统光标
        ShowCursor(TRUE);
    }
}

LRESULT Window::WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {

    auto &buffer = m_rawInputBuffer;

    switch (msg) {
    case WM_ENTERSIZEMOVE:
        // 用户开始拖拽窗口，进入模态大小调整
        m_InSizeMove = true;
        m_SizeChangedDuringMove = false;
        return 0;

    case WM_SIZE:
        if (wParam == SIZE_MINIMIZED)
            return 0;

        {
            uint32_t newWidth = LOWORD(lParam);
            uint32_t newHeight = HIWORD(lParam);

            // 仅当尺寸真正变化时处理
            if (newWidth != m_Width || newHeight != m_Height) {
                m_Width = newWidth;
                m_Height = newHeight;

                if (!m_InSizeMove) {
                    // 非模态大小调整（全屏、最大化等）：立即发送事件
                    PostWindowResizeEvent(m_Width, m_Height);
                } else {
                    // 拖拽期间尺寸变化，记录标志
                    m_SizeChangedDuringMove = true;
                }
            }
        }
        return 0;

    case WM_CLOSE:
        m_ShouldClose = true;
        DestroyWindow(hWnd);
        return 0;

    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;

    case WM_EXITSIZEMOVE:
        // 用户拖拽结束，仅当尺寸实际变化时发送事件
        if (m_SizeChangedDuringMove) {
            PostWindowResizeEvent(m_Width, m_Height);
        }
        m_InSizeMove = false;
        return 0;

    case WM_KEYDOWN:
    case WM_SYSKEYDOWN: {
        buffer.OnKeyDown(static_cast<Input::EKeyCode>(wParam));
        return 0;
    }

    case WM_KEYUP:
    case WM_SYSKEYUP: {
        buffer.OnKeyUp(static_cast<Input::EKeyCode>(wParam));
        return 0;
    }

        // ==========================
    // 鼠标输入
    // ==========================
    case WM_MOUSEMOVE: {
        int x = GET_X_LPARAM(lParam);
        int y = GET_Y_LPARAM(lParam);
        buffer.OnMouseMove(x, y);
        return 0;
    }

    case WM_MOUSEWHEEL: {
        int delta = GET_WHEEL_DELTA_WPARAM(wParam);
        buffer.OnMouseWheel(delta);
        return 0;
    }

    case WM_LBUTTONDOWN:
        buffer.OnKeyDown(Input::EKeyCode::Mouse_Left);
        return 0;
    case WM_LBUTTONUP:
        buffer.OnKeyUp(Input::EKeyCode::Mouse_Left);
        return 0;

    case WM_RBUTTONDOWN:
        buffer.OnKeyDown(Input::EKeyCode::Mouse_Right);
        return 0;
    case WM_RBUTTONUP:
        buffer.OnKeyUp(Input::EKeyCode::Mouse_Right);
        return 0;

    case WM_MBUTTONDOWN:
        buffer.OnKeyDown(Input::EKeyCode::Mouse_Middle);
        return 0;
    case WM_MBUTTONUP:
        buffer.OnKeyUp(Input::EKeyCode::Mouse_Middle);
        return 0;

    case WM_XBUTTONDOWN: {
        WORD xButton = HIWORD(wParam);
        if (xButton == XBUTTON1)
            buffer.OnKeyDown(Input::EKeyCode::Mouse_X1);
        else if (xButton == XBUTTON2)
            buffer.OnKeyDown(Input::EKeyCode::Mouse_X2);
        return TRUE;
    }
    case WM_XBUTTONUP: {
        WORD xButton = HIWORD(wParam);
        if (xButton == XBUTTON1)
            buffer.OnKeyUp(Input::EKeyCode::Mouse_X1);
        else if (xButton == XBUTTON2)
            buffer.OnKeyUp(Input::EKeyCode::Mouse_X2);
        return TRUE;
    }

    case WM_COMMAND:
        return DefWindowProcW(hWnd, msg, wParam, lParam);

    case WM_ACTIVATE:
        if (LOWORD(wParam) == WA_INACTIVE) {
            // 窗口失去焦点，重置所有输入状态，防止“卡键”
            if (m_cursorCaptured) {
                SetCursorCapture(false);
            }
            m_rawInputBuffer.Reset();
        }
        return 0;
    }

    return DefWindowProcW(hWnd, msg, wParam, lParam);
}

void Window::PostWindowResizeEvent(uint32_t width, uint32_t height) {
    auto *dispatcher = Event::MessageDispatcher::GetInstance();
    if (!dispatcher) {
        return;
    }

    dispatcher->PostEvent(Event::WindowResizeEvent::StaticTypeHash, 0, width, height, Event::EventPriority::P1_High);
}

} // namespace DX12Engine::Platform
