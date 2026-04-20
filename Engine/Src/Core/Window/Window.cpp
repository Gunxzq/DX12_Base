#include "Core/Window/Window.h"

Window::Window(const Desc &desc) : m_Desc(desc) {}

Window::~Window() {
    if (m_hWnd) {
        DestroyWindow(m_hWnd);
        m_hWnd = nullptr;
    }
    // 娉ㄦ剰锛氶€氬父涓嶉渶瑕?UnregisterClass锛岄櫎闈炴槸 DLL 棰戠箒鍔犺浇鍗歌浇
}

bool Window::Create() {
    m_hInstance = GetModuleHandle(nullptr);

    // 1. 鍑嗗绐楀彛绫?
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(WNDCLASSEX);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WndProcStatic; // 缁戝畾闈欐€佸洖璋?
    wc.hInstance = m_hInstance;

    // 浠庤祫婧愬姞杞藉浘鏍?(淇濈暀浜嗕綘鍘熶唬鐮佺殑璧勬簮寮曠敤鏂瑰紡)
    wc.hIcon = LoadIcon(m_hInstance, MAKEINTRESOURCE(IDI_DX12BASE));
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName = L"DX12WindowClass"; // 纭紪鐮佺被鍚嶏紝閬垮厤鍏ㄥ眬瀛楃涓?
    wc.hIconSm = LoadIcon(m_hInstance, MAKEINTRESOURCE(IDI_SMALL));

    if (!RegisterClassExW(&wc)) {
        return false;
    }

    // 2. 璁＄畻绐楀彛椋庢牸
    DWORD style = WS_OVERLAPPEDWINDOW;
    if (!m_Desc.resizable) {
        style &= ~WS_THICKFRAME;
        style &= ~WS_MAXIMIZEBOX;
    }

    // 3. 璁＄畻绐楀彛澶у皬 (澶勭悊杈规)
    RECT rect = {0, 0, static_cast<LONG>(m_Desc.width), static_cast<LONG>(m_Desc.height)};
    AdjustWindowRect(&rect, style, FALSE);

    // 4. 鍒涘缓绐楀彛
    // 鍏抽敭锛氶€氳繃 lpCreateParams 浼犻€?this 鎸囬拡

      m_hWnd = CreateWindowExW(0, L"DX12WindowClass", m_Desc.title.c_str(), style, CW_USEDEFAULT, CW_USEDEFAULT,
                             rect.right - rect.left, rect.bottom - rect.top, nullptr, nullptr, m_hInstance, this);

    if (!m_hWnd) {
        return false;
    }

    // 鍒濆鍖栧昂瀵?
    m_Width = m_Desc.width;
    m_Height = m_Desc.height;

    ShowWindow(m_hWnd, SW_SHOW);
    UpdateWindow(m_hWnd);

    return true;
}

void Window::ProcessMessages() {
    MSG msg = {};
    // 浣跨敤 PeekMessage 瀹炵幇闈為樆濉炴秷鎭惊鐜紝閫傚悎娓告垙寰幆
    while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
}

// 闈欐€佸洖璋冿細杩欐槸 Windows 绯荤粺璋冪敤鐨勫叆鍙?
LRESULT CALLBACK Window::WndProcStatic(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    // 濡傛灉鏄?WM_NCCREATE锛學indows 浼氭妸 CreateWindow 鐨勬渶鍚庝竴涓弬鏁?(this) 鏀惧湪
    // lParam 閲?
    if (msg == WM_NCCREATE) {
        CREATESTRUCT *pCreate = reinterpret_cast<CREATESTRUCT *>(lParam);
        Window *pThis = reinterpret_cast<Window *>(pCreate->lpCreateParams);

        // 灏嗚繖涓?this 鎸囬拡涓庣獥鍙ｅ彞鏌勭粦瀹氾紝瀛樺叆 GWLP_USERDATA
        SetWindowLongPtr(hWnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(pThis));

        return DefWindowProc(hWnd, msg, wParam, lParam);
    }

    // 浠庣獥鍙ｅ睘鎬т腑鍙栧嚭 this 鎸囬拡
    Window *pThis = reinterpret_cast<Window *>(GetWindowLongPtr(hWnd, GWLP_USERDATA));
    if (pThis) {
        return pThis->WndProcHandler(hWnd, msg, wParam, lParam);
    }

    return DefWindowProc(hWnd, msg, wParam, lParam);
}

// 鎴愬憳鍑芥暟锛氱湡姝ｇ殑閫昏緫澶勭悊
LRESULT Window::WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_SIZE:
        // 鏇存柊灏哄锛岄伩鍏嶆渶灏忓寲鏃剁殑 0 灏哄闂
        if (wParam != SIZE_MINIMIZED) {
            m_Width = LOWORD(lParam);
            m_Height = HIWORD(lParam);
        }
        return 0;

    case WM_CLOSE:
        m_ShouldClose = true;
        return 0;

    case WM_DESTROY:
        // 铏界劧 WM_CLOSE 澶勭悊浜嗛€€鍑猴紝浣?WM_DESTROY
        // 鏄郴缁熷己鍒堕攢姣佹椂鐨勬渶鍚庢満浼?
        PostQuitMessage(0);
        return 0;

    case WM_COMMAND:
        // 濡傛灉闇€瑕佸鐞嗚彍鍗曪紝鍙互淇濈暀杩欓儴鍒嗛€昏緫
        // 浣嗗缓璁€氳繃鍥炶皟閫氱煡 Bootstrap锛岃€屼笉鏄湪杩欓噷纭紪鐮?
        return DefWindowProc(hWnd, msg, wParam, lParam);
    }

    return DefWindowProc(hWnd, msg, wParam, lParam);
}