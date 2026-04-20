#include "DX12_Base.h"
#include "Core/Window/Window.h"
#include "framework.h"

int APIENTRY wWinMain(_In_ HINSTANCE hInstance, _In_opt_ HINSTANCE hPrevInstance, _In_ LPWSTR lpCmdLine,
                      _In_ int nCmdShow) {
    UNREFERENCED_PARAMETER(hInstance);
    UNREFERENCED_PARAMETER(hPrevInstance);
    UNREFERENCED_PARAMETER(lpCmdLine);
    UNREFERENCED_PARAMETER(nCmdShow);

    // 1. 创建窗口
    Window::Desc desc;
    desc.title = L"DX12 Base";
    desc.width = 1280;
    desc.height = 720;
    desc.resizable = true;

    Window window(desc);
    if (!window.Create()) {
        return -1;
    }

    // 2. 主消息循环
    while (!window.ShouldClose()) {
        window.ProcessMessages();

        // TODO: 在此处添加游戏逻辑和渲染代码
    }

    return 0;
}
