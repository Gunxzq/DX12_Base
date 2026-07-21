#include "main.h"
#include "Boot/Bootstrap.h"
#include "Boot/GameContext.h"
#include "ProjectConfigGenerated.h"
#include "Core/Editor.h"
#include "framework.h"

using namespace DX12Engine::Boot;

int APIENTRY wWinMain(_In_ HINSTANCE hInstance, _In_opt_ HINSTANCE hPrevInstance, _In_ LPWSTR lpCmdLine,
                      _In_ int nCmdShow) {
    UNREFERENCED_PARAMETER(hInstance);
    UNREFERENCED_PARAMETER(hPrevInstance);
    UNREFERENCED_PARAMETER(lpCmdLine);
    UNREFERENCED_PARAMETER(nCmdShow);

    // 设置工作目录为 exe 所在目录
    wchar_t exePath[MAX_PATH];
    GetModuleFileNameW(NULL, exePath, MAX_PATH);
    std::wstring exeDir = exePath;
    exeDir = exeDir.substr(0, exeDir.find_last_of(L"\\/"));
    SetCurrentDirectoryW(exeDir.c_str());

    Bootstrap bootstrap;

    try {
        auto projectConfig = DX12Engine::Core::GetProjectConfig();

        bootstrap.Run(projectConfig);

        GameContext *context = bootstrap.CreateContext();

        auto editor = std::make_unique<Editor>(context);

        if (!editor->Initialize()) {
            return -1;
        }

        editor->Run();

    } catch (const std::exception &e) {
        MessageBoxA(nullptr, e.what(), "Fatal Error", MB_ICONERROR);
        return -1;
    }

    return 0;
}