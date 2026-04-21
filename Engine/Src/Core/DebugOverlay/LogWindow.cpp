#include "Core/DebugOverlay/LogWindow.h"
#include <Richedit.h>

namespace DX12Engine {
namespace Core {

LogWindow *LogWindow::s_instance = nullptr;

LogWindow::LogWindow() { s_instance = this; }

LogWindow::~LogWindow() {
    if (m_editWnd) {
        DestroyWindow(m_editWnd);
        m_editWnd = nullptr;
    }
    if (m_hWnd) {
        DestroyWindow(m_hWnd);
        m_hWnd = nullptr;
    }
    UnregisterClassW(m_className.c_str(), GetModuleHandle(nullptr));
    s_instance = nullptr;
}

LogWindow *LogWindow::GetInstance() { return s_instance; }

void LogWindow::RegisterWindowClass() {
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(WNDCLASSEXW);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = GetModuleHandle(nullptr);
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName = m_className.c_str();

    RegisterClassExW(&wc);
}

void LogWindow::CreateWindowHandle() {
    RegisterWindowClass();

    m_hWnd = CreateWindowExW(0, m_className.c_str(), L"DX12 Engine Log", WS_OVERLAPPEDWINDOW | WS_VISIBLE, CW_USEDEFAULT,
                             CW_USEDEFAULT, 800, 400, nullptr, nullptr, GetModuleHandle(nullptr), this);

    if (!m_hWnd) {
        return;
    }

    // 创建只读的多行编辑框
    m_editWnd = CreateWindowExW(
        0, L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_LEFT | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL, 0, 0,
        0, 0, // 稍后调整大小
        m_hWnd, nullptr, GetModuleHandle(nullptr), nullptr);

    if (!m_editWnd) {
        return;
    }

    // 设置字体
    HFONT hFont = CreateFontW(14, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                              CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY, FIXED_PITCH, L"Consolas");
    if (hFont) {
        SendMessageW(m_editWnd, WM_SETFONT, (WPARAM)hFont, TRUE);
    }

    ResizeControls();
}

void LogWindow::ResizeControls() {
    if (!m_hWnd || !m_editWnd)
        return;

    RECT clientRect;
    GetClientRect(m_hWnd, &clientRect);
    SetWindowPos(m_editWnd, nullptr, 0, 0, clientRect.right, clientRect.bottom, SWP_NOZORDER);
}

void LogWindow::Show() {
    if (!m_hWnd) {
        CreateWindowHandle();
    }
    if (m_hWnd) {
        ShowWindow(m_hWnd, SW_SHOW);
        UpdateWindow(m_hWnd);
    }
}

void LogWindow::Hide() {
    if (m_hWnd) {
        ShowWindow(m_hWnd, SW_HIDE);
    }
}

void LogWindow::Toggle() {
    if (!m_hWnd) {
        Show();
    } else if (IsWindowVisible(m_hWnd)) {
        Hide();
    } else {
        ShowWindow(m_hWnd, SW_SHOW);
        UpdateWindow(m_hWnd);
    }
}

// 将 UTF-8 字符串转换为 UTF-16
static std::wstring Utf8ToUtf16(const std::string &utf8) {
    if (utf8.empty())
        return {};
    int size = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, nullptr, 0);
    std::wstring result(size - 1, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, result.data(), size);
    return result;
}

void LogWindow::AppendLog(const std::string &text) {
    if (!m_editWnd)
        return;

    std::wstring wtext = Utf8ToUtf16(text);
    int len = GetWindowTextLengthW(m_editWnd);
    SendMessageW(m_editWnd, EM_SETSEL, (WPARAM)len, (LPARAM)len);
    SendMessageW(m_editWnd, EM_REPLACESEL, FALSE, (LPARAM)wtext.c_str());

    // 自动滚动到底部
    SendMessageW(m_editWnd, EM_SCROLLCARET, 0, 0);
}

void LogWindow::Clear() {
    if (m_editWnd) {
        SetWindowTextW(m_editWnd, L"");
    }
}

void LogWindow::UpdateScrollBar() {
    if (!m_editWnd)
        return;

    SCROLLINFO si = {};
    si.cbSize = sizeof(SCROLLINFO);
    si.fMask = SIF_ALL;
    GetScrollInfo(m_editWnd, SB_VERT, &si);
}

void LogWindow::EnsureVisible() {
    // 自动滚动到底部
    int len = GetWindowTextLengthW(m_editWnd);
    SendMessageW(m_editWnd, EM_SETSEL, (WPARAM)len, (LPARAM)len);
    SendMessageW(m_editWnd, EM_SCROLLCARET, 0, 0);
}

LRESULT CALLBACK LogWindow::WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_SIZE: {
        LogWindow *pThis = reinterpret_cast<LogWindow *>(GetWindowLongPtr(hWnd, GWLP_USERDATA));
        if (pThis) {
            RECT clientRect;
            GetClientRect(hWnd, &clientRect);
            SetWindowPos(pThis->m_editWnd, nullptr, 0, 0, clientRect.right, clientRect.bottom, SWP_NOZORDER);
        }
        return 0;
    }
    case WM_CLOSE:
        ShowWindow(hWnd, SW_HIDE);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    case WM_NCCREATE: {
        CREATESTRUCT *pCreate = reinterpret_cast<CREATESTRUCT *>(lParam);
        LogWindow *pThis = reinterpret_cast<LogWindow *>(pCreate->lpCreateParams);
        SetWindowLongPtr(hWnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(pThis));
        return DefWindowProcW(hWnd, msg, wParam, lParam);
    }
    default:
        return DefWindowProcW(hWnd, msg, wParam, lParam);
    }
}

} // namespace Core
} // namespace DX12Engine
