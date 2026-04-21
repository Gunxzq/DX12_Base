#include "Core/Window/Window.h"

namespace DX12Engine {
namespace Core {

Window::Window(const Desc &desc) : m_Desc(desc) {}

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
    if (!m_Desc.resizable) {
        style &= ~WS_THICKFRAME;
        style &= ~WS_MAXIMIZEBOX;
    }

    RECT rect = {0, 0, static_cast<LONG>(m_Desc.width), static_cast<LONG>(m_Desc.height)};
    AdjustWindowRect(&rect, style, FALSE);

    m_hWnd = CreateWindowExW(0, L"DX12WindowClass", m_Desc.title.c_str(), style, CW_USEDEFAULT, CW_USEDEFAULT,
                             rect.right - rect.left, rect.bottom - rect.top, nullptr, nullptr, m_hInstance, this);

    if (!m_hWnd)
        return false;

    // 设置窗口位置
    SetWindowPos(m_hWnd, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);

    m_Width = m_Desc.width;
    m_Height = m_Desc.height;

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

LRESULT Window::WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_SIZE:

        if (wParam != SIZE_MINIMIZED) {
            m_Width = LOWORD(lParam);
            m_Height = HIWORD(lParam);
        }
        return 0;

    case WM_CLOSE:
        m_ShouldClose = true;
        DestroyWindow(hWnd); // 显式销毁窗口，触发 WM_DESTROY → PostQuitMessage
        return 0;

    case WM_DESTROY:

        // 发生一个终止消息循环的消息
        PostQuitMessage(0);
        return 0;

    case WM_COMMAND:

        return DefWindowProcW(hWnd, msg, wParam, lParam);
    }

    return DefWindowProcW(hWnd, msg, wParam, lParam);
}

} // namespace Core
} // namespace DX12Engine
