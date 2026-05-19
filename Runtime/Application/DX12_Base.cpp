#include "DX12_Base.h"
#include "Boot/Bootstrap.h"
#include "Boot/GameContext.h"
#include "Game/Game.h"
#include "framework.h"

int APIENTRY wWinMain(_In_ HINSTANCE hInstance, _In_opt_ HINSTANCE hPrevInstance, _In_ LPWSTR lpCmdLine,
                      _In_ int nCmdShow) {
    UNREFERENCED_PARAMETER(hInstance);
    UNREFERENCED_PARAMETER(hPrevInstance);
    UNREFERENCED_PARAMETER(lpCmdLine);
    UNREFERENCED_PARAMETER(nCmdShow);

    DX12Engine::Core::Bootstrap bootstrap;

    try {
        // 初始化模块
        bootstrap.Run();

        DX12Engine::Core::GameContext *context = bootstrap.CreateContext();

        // Game 层直接使用 context->FrameDriver（由 Bootstrap 创建）
        auto game = std::make_unique<Game>(context);

        // 初始化游戏逻辑
        if (!game->Initialize()) {
            return -1;
        }

        // 运行主循环 (阻塞直到退出)
        game->Run();

    } catch (const std::exception &e) {
        MessageBoxA(nullptr, e.what(), "Fatal Error", MB_ICONERROR);
        return -1;
    }

    // 6. 清理 (unique_ptr 自动析构 Game, Bootstrap 析构时清理基础设施)
    return 0;
}
