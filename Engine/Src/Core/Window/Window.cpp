#include "Core/Window/Window.h"

Window::Window(const Desc &desc) : m_Desc(desc) {}

Window::~Window() {
    if (m_hWnd) {
        DestroyWindow(m_hWnd);
        m_hWnd = nullptr;
    }
}

bool Window::Create() {
    m_hInstance = GetModuleHandle(nullptr);

    // 1. 鍑嗗绐楀彛绫?
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(WNDCLASSEX);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WndProcStatic;
    wc.hInstance = m_hInstance;

    wc.hIcon = LoadIcon(m_hInstance, MAKEINTRESOURCE(IDI_DX12BASE));
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName = L"DX12WindowClass";
    wc.hIconSm = LoadIcon(m_hInstance, MAKEINTRESOURCE(IDI_SMALL));

    if (!RegisterClassExW(&wc)) {
        return false;
    }

    DWORD style = WS_OVERLAPPEDWINDOW;
    if (!m_Desc.resizable) {
        style &= ~WS_THICKFRAME;
        style &= ~WS_MAXIMIZEBOX;
    }

    RECT rect = {0, 0, static_cast<LONG>(m_Desc.width), static_cast<LONG>(m_Desc.height)};
    AdjustWindowRect(&rect, style, FALSE);

    m_hWnd = CreateWindowExW(0, L"DX12WindowClass", m_Desc.title.c_str(), style, CW_USEDEFAULT, CW_USEDEFAULT,
                             rect.right - rect.left, rect.bottom - rect.top, nullptr, nullptr, m_hInstance, this);

    if (!m_hWnd) {
        return false;
    }
    SetWindowTextW(m_hWnd, m_Desc.title.c_str());

    SetWindowPos(m_hWnd, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);

    m_Width = m_Desc.width;
    m_Height = m_Desc.height;

    ShowWindow(m_hWnd, SW_SHOW);
    UpdateWindow(m_hWnd);

    return true;
}

void Window::ProcessMessages() {
    MSG msg = {};

    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
}

LRESULT CALLBACK Window::WndProcStatic(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {

    if (msg == WM_NCCREATE) {
        CREATESTRUCT *pCreate = reinterpret_cast<CREATESTRUCT *>(lParam);
        Window *pThis = reinterpret_cast<Window *>(pCreate->lpCreateParams);

        SetWindowLongPtr(hWnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(pThis));

        return DefWindowProcW(hWnd, msg, wParam, lParam);
    }

    Window *pThis = reinterpret_cast<Window *>(GetWindowLongPtr(hWnd, GWLP_USERDATA));
    if (pThis) {
        return pThis->WndProcHandler(hWnd, msg, wParam, lParam);
    }

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
        return 0;

    case WM_DESTROY:

        PostQuitMessage(0);
        return 0;

    case WM_COMMAND:

        return DefWindowProcW(hWnd, msg, wParam, lParam);
    }

    return DefWindowProcW(hWnd, msg, wParam, lParam);
}