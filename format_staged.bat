@echo off
setlocal

:: 配置你的 clang-format 路径
set CLANG_FORMAT_PATH=D:\Dev\vs2022\VC\Tools\Llvm\bin\clang-format.exe

:: 检查 clang-format 是否存在
if not exist "%CLANG_FORMAT_PATH%" (
    echo Error: clang-format not found at %CLANG_FORMAT_PATH%
    exit /b 1
)

:: 获取暂存区(staged)中所有 .cpp, .h, .hpp, .inl, .c 文件
:: git diff --cached --name-only --diff-filter=ACM 列出暂存区中新增、复制、修改的文件
for /f "usebackq delims=" %%f in (`git diff --cached --name-only --diff-filter=ACM ^| findstr /R "\.cpp$ \.h$ \.hpp$ \.inl$ \.c$"`) do (
    if exist "%%f" (
        echo Formatting: %%f
        "%CLANG_FORMAT_PATH%" -i "%%f"
        :: 格式化后必须重新 add，否则提交的还是旧格式
        git add "%%f"
    )
)

echo Formatting complete.
exit /b 0