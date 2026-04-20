#pragma once
#include "Resource.h"
#include <Windows.h>
#include <functional>
#include <string>

class Window {
public:
    // 配置结构体，替代硬编码的字符串
    struct Desc {
        std::wstring title = L"DX12 Engine";
        uint32_t width = 1280;
        uint32_t height = 720;
        bool resizable = true;
    };

public:
    Window(const Desc &desc);
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

private:
    // 静态 WndProc，用于接收 Windows 消息
    static LRESULT CALLBACK WndProcStatic(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);
    // 成员 WndProc，处理具体逻辑
    LRESULT WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

private:
    HWND m_hWnd = nullptr;
    HINSTANCE m_hInstance = nullptr;
    Desc m_Desc;

    // 运行时状态
    uint32_t m_Width = 0;
    uint32_t m_Height = 0;
    bool m_ShouldClose = false;
};