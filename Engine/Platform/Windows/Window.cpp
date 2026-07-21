#include "Window.h"

#include "Common/Common.h"
#include "Resource.h"

#include "Core/Config/ConfigTypes/WindowConfig.h"
#include "Event/MessageDispatcher.h"
#include "Platform/Input/InputManager.h"
#include "Platform/Input/RawInputBuffer.h"
#include "ThirdParty/imgui/backends/imgui_impl_dx12.h"
#include "ThirdParty/imgui/backends/imgui_impl_win32.h"
#include "ThirdParty/imgui/imgui.h"
#include <Windows.h>
#include <Windowsx.h>

// 外部声明
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

using namespace DX12Engine::Boot;
using namespace DX12Engine::Event;
using DX12Engine::Input::RawInputBuffer;

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
    if (m_IsFullscreen) {
        // 无边框窗口模式
        style = WS_POPUP;
        // 获取显示器实际的宽高
        m_InitWidth = GetSystemMetrics(SM_CXSCREEN);
        m_InitHeight = GetSystemMetrics(SM_CYSCREEN);
    } else {
        // 普通窗口模式：可调整大小
        style = WS_OVERLAPPEDWINDOW;
        if (!m_IsResizable) {
            style &= ~WS_THICKFRAME;
            style &= ~WS_MAXIMIZEBOX;
        }
    }

    RECT rect = {0, 0, static_cast<LONG>(m_InitWidth), static_cast<LONG>(m_InitHeight)};
    AdjustWindowRect(&rect, style, FALSE);

    m_hWnd = CreateWindowExW(m_IsFullscreen ? WS_EX_TOPMOST : 0, // 全屏时可置于顶层
                             L"DX12WindowClass", m_Title.c_str(), style,
                             m_IsFullscreen ? 0 : CW_USEDEFAULT, // 全屏时左上角在 (0,0)
                             m_IsFullscreen ? 0 : CW_USEDEFAULT, rect.right - rect.left, rect.bottom - rect.top,
                             nullptr, nullptr, m_hInstance, this);
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

void Window::SetInputManager(Input::InputManager *inputMgr) { m_inputMgr = inputMgr; }

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

        if (m_inputMgr) {
            m_inputMgr->ResetAllStates();
        }

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

    } else {
        // 1. 释放捕获
        ReleaseCapture();

        // 2. 显示系统光标
        ShowCursor(TRUE);
    }
}

LRESULT Window::WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {

    switch (m_inputPriorityIsImGuiFirst ? 0 : 1) {
    case 0:
    default:
        ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam);
        ProcessInputMessage(msg, wParam, lParam);
        break;

    case 1:
        ProcessInputMessage(msg, wParam, lParam);
        ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam);
        break;
    }

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
    case WM_SYSKEYDOWN:
        return 0;

    case WM_KEYUP:
    case WM_SYSKEYUP:
        return 0;

        // ==========================
    // 鼠标输入
    // ==========================
    case WM_MOUSEMOVE:
        return 0;

    case WM_MOUSEWHEEL:
        return 0;

    case WM_LBUTTONDOWN:
        return 0;
    case WM_LBUTTONUP:
        return 0;

    case WM_RBUTTONDOWN:
        return 0;
    case WM_RBUTTONUP:
        return 0;

    case WM_MBUTTONDOWN:
        return 0;
    case WM_MBUTTONUP:
        return 0;

    case WM_XBUTTONDOWN:
        return TRUE;
    case WM_XBUTTONUP:
        return TRUE;

    case WM_COMMAND:
        return DefWindowProcW(hWnd, msg, wParam, lParam);

    case WM_ACTIVATE:
        if (LOWORD(wParam) == WA_INACTIVE) {
            // 窗口失去焦点，重置所有输入状态，防止“卡键”
            if (m_cursorCaptured) {
                SetCursorCapture(false);
            }
            m_inputMgr->GetRawBuffer()->Reset();
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
void Window::SetFullscreen(bool fullscreen) {
    if (m_IsFullscreen == fullscreen)
        return;

    m_IsFullscreen = fullscreen;

    if (fullscreen) {
        // 保存当前窗口的位置和大小，以便恢复
        GetWindowRect(m_hWnd, &m_WindowedRect);

        // 获取屏幕大小
        int screenWidth = GetSystemMetrics(SM_CXSCREEN);
        int screenHeight = GetSystemMetrics(SM_CYSCREEN);

        // 移除边框样式
        SetWindowLong(m_hWnd, GWL_STYLE, WS_POPUP);
        // 设置窗口位置和大小覆盖整个屏幕
        SetWindowPos(m_hWnd, HWND_TOPMOST, 0, 0, screenWidth, screenHeight, SWP_FRAMECHANGED | SWP_SHOWWINDOW);
    } else {
        // 恢复边框样式
        SetWindowLong(m_hWnd, GWL_STYLE, WS_OVERLAPPEDWINDOW);
        // 恢复到窗口模式时的位置和大小
        SetWindowPos(m_hWnd, HWND_NOTOPMOST, m_WindowedRect.left, m_WindowedRect.top,
                     m_WindowedRect.right - m_WindowedRect.left, m_WindowedRect.bottom - m_WindowedRect.top,
                     SWP_FRAMECHANGED | SWP_SHOWWINDOW);
    }

    // 通知渲染系统：窗口大小已改变，需要重建交换链
    PostWindowResizeEvent(m_Width, m_Height);
}

// ========================================================================
// 输入消息处理：由 WndProcHandler 根据优先级调用
// 将 Windows 消息转发到 InputManager
// ========================================================================

void Window::ProcessInputMessage(UINT msg, WPARAM wParam, LPARAM lParam) {
    if (!m_inputMgr)
        return;

    switch (msg) {
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
        m_inputMgr->GetRawBuffer()->OnKeyDown(static_cast<Input::EKeyCode>(wParam));
        break;

    case WM_KEYUP:
    case WM_SYSKEYUP:
        m_inputMgr->GetRawBuffer()->OnKeyUp(static_cast<Input::EKeyCode>(wParam));
        break;

    case WM_MOUSEMOVE: {
        int x = GET_X_LPARAM(lParam);
        int y = GET_Y_LPARAM(lParam);
        m_mouseX = x;
        m_mouseY = y;
        m_inputMgr->GetRawBuffer()->OnMouseMove(x, y);
        break;
    }

    case WM_MOUSEWHEEL: {
        int delta = GET_WHEEL_DELTA_WPARAM(wParam);
        m_inputMgr->GetRawBuffer()->OnMouseWheel(delta);
        break;
    }

    case WM_LBUTTONDOWN:
        m_inputMgr->GetRawBuffer()->OnKeyDown(Input::EKeyCode::Mouse_Left);
        break;
    case WM_LBUTTONUP:
        m_inputMgr->GetRawBuffer()->OnKeyUp(Input::EKeyCode::Mouse_Left);
        break;

    case WM_RBUTTONDOWN:
        m_inputMgr->GetRawBuffer()->OnKeyDown(Input::EKeyCode::Mouse_Right);
        break;
    case WM_RBUTTONUP:
        m_inputMgr->GetRawBuffer()->OnKeyUp(Input::EKeyCode::Mouse_Right);
        break;

    case WM_MBUTTONDOWN:
        m_inputMgr->GetRawBuffer()->OnKeyDown(Input::EKeyCode::Mouse_Middle);
        break;
    case WM_MBUTTONUP:
        m_inputMgr->GetRawBuffer()->OnKeyUp(Input::EKeyCode::Mouse_Middle);
        break;

    case WM_XBUTTONDOWN: {
        WORD xButton = HIWORD(wParam);
        if (xButton == XBUTTON1)
            m_inputMgr->GetRawBuffer()->OnKeyDown(Input::EKeyCode::Mouse_X1);
        else if (xButton == XBUTTON2)
            m_inputMgr->GetRawBuffer()->OnKeyDown(Input::EKeyCode::Mouse_X2);
        break;
    }
    case WM_XBUTTONUP: {
        WORD xButton = HIWORD(wParam);
        if (xButton == XBUTTON1)
            m_inputMgr->GetRawBuffer()->OnKeyUp(Input::EKeyCode::Mouse_X1);
        else if (xButton == XBUTTON2)
            m_inputMgr->GetRawBuffer()->OnKeyUp(Input::EKeyCode::Mouse_X2);
        break;
    }
    }
}

} // namespace DX12Engine::Platform
