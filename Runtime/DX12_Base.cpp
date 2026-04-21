#include "DX12_Base.h"
#include "Core/Bootstrap/Bootstrap.h"
#include "framework.h"

int APIENTRY wWinMain(_In_ HINSTANCE hInstance, _In_opt_ HINSTANCE hPrevInstance, _In_ LPWSTR lpCmdLine,
                      _In_ int nCmdShow) {
    UNREFERENCED_PARAMETER(hInstance);
    UNREFERENCED_PARAMETER(hPrevInstance);
    UNREFERENCED_PARAMETER(lpCmdLine);
    UNREFERENCED_PARAMETER(nCmdShow);

    // 创建启动模块
    DX12Engine::Core::Bootstrap bootstrap;

    // 初始化（加载配置，创建窗口）
    if (!bootstrap.Initialize("Config")) {
        return -1;
    }

    // 运行主循环
    bootstrap.Run();

    // 关闭并清理
    bootstrap.Shutdown();

    return 0;
}
