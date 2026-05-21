#include "DebugOverlay.h"

#include <algorithm>

namespace DX12Engine::Logger {

LogEntry::LogEntry(Level lvl, std::string msg, std::string fmt)
    : timestamp(std::chrono::steady_clock::now()), level(lvl), message(std::move(msg)), formatted(std::move(fmt)) {}

DebugOverlay *DebugOverlay::GetInstance() {
    if (!s_instance) {
        s_instance = new DebugOverlay();
    }
    return s_instance;
}

DebugOverlay::~DebugOverlay() { Destroy(); }

void DebugOverlay::PushLog(LogEntry::Level level, const std::string &message, const std::string &formatted) {
    {
        std::lock_guard<std::mutex> lock(m_queueMutex);
        m_incomingQueue.emplace_back(level, message, formatted);
    }

    // 如果窗口可见且队列积压不多，可以尝试直接触发更新（可选优化）
    // 这里主要依赖外部调用 Update 或定时器

    if (m_hwnd) {
        PostMessageW(m_hwnd, WM_LOG_UPDATE, 0, 0);
    }
}

void DebugOverlay::Show() {
    if (m_hwnd) {
        ShowWindow(m_hwnd, SW_SHOW);
        m_visible = true;
        return;
    }

    // 注册窗口类
    WNDCLASSEX wc = {};
    wc.cbSize = sizeof(WNDCLASSEX);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = GetModuleHandle(nullptr);
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName = L"DX12EngineLogWindow";
    wc.hIcon = LoadIcon(nullptr, IDI_APPLICATION);

    RegisterClassEx(&wc);

    // 创建窗口
    m_hwnd = CreateWindowEx(0, L"DX12EngineLogWindow", L"Engine Log", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                            800, 600, nullptr, nullptr, GetModuleHandle(nullptr),
                            this // 传递 this 指针到 lpParam
    );

    if (m_hwnd) {
        ShowWindow(m_hwnd, SW_SHOW);
        UpdateWindow(m_hwnd);
        m_visible = true;
    }
}

void DebugOverlay::Hide() {
    if (m_hwnd) {
        ShowWindow(m_hwnd, SW_HIDE);
        m_visible = false;
    }
}

void DebugOverlay::Destroy() {
    if (m_hwnd) {
        DestroyWindow(m_hwnd);
        m_hwnd = nullptr;
        m_hEdit = nullptr;
        m_visible = false;
    }
}

void DebugOverlay::ProcessQueue() {
    if (!m_hwnd || !m_hEdit)
        return;

    std::lock_guard<std::mutex> lock(m_queueMutex);
    if (m_incomingQueue.empty())
        return;

    std::wstring appendText;
    // 预分配一些空间以减少 realloc，可选优化
    appendText.reserve(1024);

    while (!m_incomingQueue.empty()) {
        LogEntry entry = std::move(m_incomingQueue.front());
        m_incomingQueue.pop_front();

        // 简单格式化：[Level] Message
        std::string levelStr;
        switch (entry.level) {
        case LogEntry::Trace:
            levelStr = "[TRACE] ";
            break;
        case LogEntry::Debug:
            levelStr = "[DEBUG] ";
            break;
        case LogEntry::Info:
            levelStr = "[INFO] ";
            break;
        case LogEntry::Warn:
            levelStr = "[WARN] ";
            break;
        case LogEntry::Error:
            levelStr = "[ERROR] ";
            break;
        case LogEntry::Critical:
            levelStr = "[CRITICAL] ";
            break;
        }

        // 组合完整行
        std::string line = entry.formatted;
        // 确保末尾有换行
        if (line.empty() || line.back() != '\n') {
            line += "\r\n";
        } else if (line.back() == '\n' && (line.size() < 2 || line[line.size() - 2] != '\r')) {
            line.insert(line.end() - 1, '\r'); // 将 \n 变为 \r\n
        }

        // 【修复点】安全地将 UTF-8 std::string 转换为 std::wstring
        int wLen = MultiByteToWideChar(CP_UTF8, 0, line.c_str(), -1, nullptr, 0);
        if (wLen > 0) {
            // 创建 wstring，大小为 wLen-1 (因为 MultiByteToWideChar 返回的长度包含 null terminator)
            std::wstring wLine(wLen - 1, L'\0');
            MultiByteToWideChar(CP_UTF8, 0, line.c_str(), -1, &wLine[0], wLen);

            // 直接追加 wstring，避免指针操作的歧义
            appendText += wLine;
        }
    }

    if (!appendText.empty()) {
        // 获取当前文本长度
        int currentLen = GetWindowTextLengthW(m_hEdit);

        // 限制总行数/长度以防止内存无限增长
        const int MAX_CHARS = 1024 * 1024; // 1MB
        if (currentLen > MAX_CHARS) {
            // 清除一半旧数据
            SendMessageW(m_hEdit, EM_SETSEL, 0, MAX_CHARS / 2);
            SendMessageW(m_hEdit, EM_REPLACESEL, FALSE, (LPARAM)L"");
            currentLen = GetWindowTextLengthW(m_hEdit);
        }

        // 追加文本
        SendMessageW(m_hEdit, EM_SETSEL, currentLen, currentLen);
        // 【注意】这里传入的是 c_str()，确保是 null-terminated 的宽字符串
        SendMessageW(m_hEdit, EM_REPLACESEL, FALSE, (LPARAM)appendText.c_str());

        // 自动滚动到底部
        SendMessageW(m_hEdit, WM_VSCROLL, SB_BOTTOM, 0);
    }
}
LRESULT CALLBACK DebugOverlay::WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    DebugOverlay *self = nullptr;

    if (msg == WM_NCCREATE) {
        CREATESTRUCT *cs = reinterpret_cast<CREATESTRUCT *>(lParam);
        self = reinterpret_cast<DebugOverlay *>(cs->lpCreateParams);
        SetWindowLongPtr(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    } else {
        self = reinterpret_cast<DebugOverlay *>(GetWindowLongPtr(hwnd, GWLP_USERDATA));
    }

    if (self) {
        return self->HandleMessage(hwnd, msg, wParam, lParam);
    }

    return DefWindowProc(hwnd, msg, wParam, lParam);
}

LRESULT DebugOverlay::HandleMessage(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_CREATE: {
        // 创建只读的多行编辑框用于显示日志
        m_hEdit = CreateWindowEx(WS_EX_CLIENTEDGE, L"EDIT", L"",
                                 WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_HSCROLL | ES_MULTILINE | ES_READONLY |
                                     ES_AUTOVSCROLL | ES_AUTOHSCROLL,
                                 0, 0, 0, 0, // 尺寸将在 WM_SIZE 中调整
                                 hwnd, nullptr, GetModuleHandle(nullptr), nullptr);

        // 设置字体
        HFONT hFont = CreateFontW(16, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                                  CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Consolas");
        SendMessageW(m_hEdit, WM_SETFONT, (WPARAM)hFont, TRUE);
        break;
    }

    case WM_LOG_UPDATE: {
        ProcessQueue();
        return 0;
    }

    case WM_SIZE: {
        if (m_hEdit) {
            MoveWindow(m_hEdit, 0, 0, LOWORD(lParam), HIWORD(lParam), TRUE);
        }
        break;
    }
    case WM_CLOSE: {
        Hide(); // 只是隐藏，不销毁
        return 0;
    }
    case WM_DESTROY: {
        m_hwnd = nullptr;
        m_hEdit = nullptr;
        PostQuitMessage(0);
        break;
    }
    }
    return DefWindowProc(hwnd, msg, wParam, lParam);
}

} // namespace DX12Engine::Logger