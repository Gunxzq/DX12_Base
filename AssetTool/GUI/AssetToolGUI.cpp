// ========================================================================
// AssetToolGUI — Win32 GUI 版 UKW 资产转换工具
//
// 功能：
//   1. 选择来源目录/输出目录
//   2. 递归扫描来源目录中所有 .hod 文件
//   3. 在输出目录中保持原有目录结构输出 JSON+TXT
//   4. 支持批量转换和进度显示
//
// 编译：
//   链接 comctl32.lib (ListView), ole32.lib (IFileDialog)
// ========================================================================

#define NOMINMAX
#define _WIN32_WINNT 0x0601

#include <windows.h> // 必须在其他 Windows 头之前

#include <algorithm>
#include <atomic>
#include <commctrl.h>
#include <filesystem>
#include <fstream>
#include <objbase.h>
#include <set>
#include <shellapi.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <shobjidl.h>
#include <sstream>
#include <stdio.h>
#include <string>
#include <thread>
#include <vector>

// 引擎头文件（DxMeshFormat 等）
#include "Asset/Definitions/Mesh/DxMeshFormat.h"
#include "Asset/IO/Writer/DxMeshWriter.h"
#include "Core/HODParser.h"
#include "Core/ANIParser.h"
#include "Core/MPDParser.h"
#include "Core/RobotMerger.h"
#include "Core/ScriptSPTParser.h"
#include "Core/TextureConverter.h"
#include "Core/XFileDirectReader.h"
#include "Core/XFileParser.h"
#include "Core/XORCipher.h"

#include <assimp/Exporter.hpp>
#include <assimp/postprocess.h>
#include <assimp/scene.h>
#include <functional>

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "shlwapi.lib")

namespace fs = std::filesystem;

// ==========================================================================
// 资源 ID
// ==========================================================================

#define IDC_MAIN_BTN_INPUT 1001
#define IDC_MAIN_BTN_OUTPUT 1002
#define IDC_MAIN_BTN_SCAN 1003
#define IDC_MAIN_BTN_CONVERT 1004
#define IDC_MAIN_EDIT_INPUT 1005
#define IDC_MAIN_EDIT_OUTPUT 1006
#define IDC_MAIN_LIST 1007
#define IDC_MAIN_PROGRESS 1008
#define IDC_MAIN_STATUS 1009
#define IDC_MAIN_LABEL_INPUT 1010
#define IDC_MAIN_LABEL_OUTPUT 1011
#define IDC_MAIN_BTN_OPEN_OUTPUT 1012
#define IDC_MAIN_CHK_JSON 1013
#define IDC_MAIN_CHK_TXT 1014
#define IDC_MAIN_CHK_DXMESH 1015
#define IDC_MAIN_BTN_CONVERT_MESH 1016
#define IDC_MAIN_BTN_CONVERT_HOD 1017
#define IDC_MAIN_BTN_MAP_BUILD 1018
#define IDC_MAIN_BTN_CONVERT_PNG 1019
#define IDC_MAIN_BTN_IMPORT_ROBOT 1020
#define IDC_MAIN_BTN_CONVERT_ANI 1022
#define IDC_MAIN_BTN_IMPORT_ANI 1023

// ==========================================================================
// 全局状态
// ==========================================================================

static HINSTANCE g_hInst = nullptr;
static HWND g_hWnd = nullptr;
static HWND g_hList = nullptr;
static HWND g_hProgress = nullptr;
static HWND g_hStatus = nullptr;
static HWND g_hBtnConvert = nullptr;
static HWND g_hBtnScan = nullptr;
static HWND g_hBtnConvertMesh = nullptr;
static HWND g_hBtnConvertHOD = nullptr;
static HWND g_hBtnMapBuild = nullptr;
static HWND g_hBtnConvertPNG = nullptr;
static HWND g_hBtnImportRobot = nullptr;
static HWND g_hBtnConvertANI = nullptr;
static HWND g_hBtnImportANI = nullptr;

static std::vector<fs::path> g_xFiles;         // 找到的 .x 完整路径
static std::vector<std::string> g_xRelPaths;   // 对应的相对路径
static std::vector<fs::path> g_hodFiles;       // 找到的 .hod 完整路径
static std::vector<std::string> g_hodRelPaths; // 对应的相对路径
static std::vector<fs::path> g_mpdFiles;       // 找到的 .mpd 完整路径
static std::vector<std::string> g_mpdRelPaths; // 对应的相对路径
static std::vector<fs::path> g_sptFiles;       // 找到的 .spt 完整路径
static std::vector<std::string> g_sptRelPaths; // 对应的相对路径
static std::vector<fs::path> g_pngFiles;       // 找到的 .png 完整路径
static std::vector<std::string> g_pngRelPaths; // 对应的相对路径
static std::vector<fs::path> g_aniFiles;       // 找到的 .ani 完整路径
static std::vector<std::string> g_aniRelPaths; // 对应的相对路径
static std::atomic<bool> g_converting{false};
static std::atomic<int> g_convertProgress{0};
static std::atomic<int> g_convertTotal{0};
static std::atomic<int> g_convertSuccess{0};
static std::atomic<int> g_convertErrors{0};

// ==========================================================================
// 辅助函数
// ==========================================================================

static std::wstring UTF8ToWide(const std::string &utf8) {
    if (utf8.empty())
        return L"";
    int len = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), (int)utf8.size(), nullptr, 0);
    std::wstring wstr(len, 0);
    MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), (int)utf8.size(), &wstr[0], len);
    return wstr;
}

static std::string WideToUTF8(const std::wstring &wstr) {
    if (wstr.empty())
        return "";
    int len = WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), (int)wstr.size(), nullptr, 0, nullptr, nullptr);
    std::string utf8(len, 0);
    WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), (int)wstr.size(), &utf8[0], len, nullptr, nullptr);
    return utf8;
}

static void SetStatusText(HWND hStatus, const std::wstring &text) {
    SetWindowTextW(hStatus, text.c_str());
    // 也在控制台输出（用于调试）
    OutputDebugStringW(text.c_str());
    OutputDebugStringW(L"\n");
}

static void SetStatusFmt(HWND hStatus, const wchar_t *fmt, ...) {
    wchar_t buf[1024];
    va_list args;
    va_start(args, fmt);
    vswprintf_s(buf, fmt, args);
    va_end(args);
    SetStatusText(hStatus, buf);
}

// ==========================================================================
// 辅助：复制或转换纹理文件（PNG → DDS 自动转换）
// ==========================================================================

/// 处理纹理文件：优先 XOR 解密 .dds，否则从 .png/.bmp 转换。
/// @return 输出文件名，空字符串表示无输出
static std::wstring CopyOrConvertTexture(const fs::path &srcDir, const std::string &texFilename,
                                         const fs::path &dstDir) {
    std::string ext = fs::path(texFilename).extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);

    if (ext == ".dds") {
        // 优先 XOR 解密原始 .dds
        fs::path srcPath = srcDir / texFilename;
        if (fs::exists(srcPath)) {
            fs::path ddsOut = dstDir / fs::path(texFilename).filename();
            auto r = AssetTool::DecryptOrCopyDDS(srcPath.string(), ddsOut.string());
            if (r.success) {
                SetStatusFmt(g_hStatus, L"[debug]   → %s (decrypted)", ddsOut.filename().wstring().c_str());
                return ddsOut.filename().wstring();
            }
        }
        // 解密失败：找同名 .png 兜底
        fs::path pngPath = srcDir / (fs::path(texFilename).stem().string() + ".png");
        if (fs::exists(pngPath)) {
            fs::path ddsPath = dstDir / (pngPath.stem().string() + ".dds");
            auto r = AssetTool::ConvertPNGToDDS(pngPath.string(), ddsPath.string());
            if (r.success)
                return ddsPath.filename().wstring();
        }
        // 也试试 .bmp
        fs::path bmpPath = srcDir / (fs::path(texFilename).stem().string() + ".bmp");
        if (fs::exists(bmpPath)) {
            fs::path ddsPath = dstDir / (bmpPath.stem().string() + ".dds");
            auto r = AssetTool::ConvertPNGToDDS(bmpPath.string(), ddsPath.string());
            if (r.success)
                return ddsPath.filename().wstring();
        }
        return L""; // 无可用源
    }

    // .png → .dds
    if (ext == ".png") {
        fs::path pngPath = srcDir / texFilename;
        if (fs::exists(pngPath)) {
            fs::path ddsPath = dstDir / (pngPath.stem().string() + ".dds");
            auto r = AssetTool::ConvertPNGToDDS(pngPath.string(), ddsPath.string());
            if (r.success)
                return ddsPath.filename().wstring();
        }
        return L"";
    }

    return L"";
}

// 记住上次选择的路径
static std::wstring g_lastInputDir;
static std::wstring g_lastOutputDir;

// ==========================================================================
// 文件夹选择对话框（带上次路径记忆）
// ==========================================================================

static bool PickFolderDialog(HWND hParent, std::wstring &outPath, const wchar_t *title,
                             const std::wstring &initialFolder) {
    // 使用 IFileDialog (Vista+)
    IFileDialog *pfd = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&pfd));
    if (FAILED(hr))
        return false;

    DWORD dwOptions;
    pfd->GetOptions(&dwOptions);
    pfd->SetOptions(dwOptions | FOS_PICKFOLDERS | FOS_PATHMUSTEXIST);

    if (title) {
        pfd->SetTitle(title);
    }

    // 设置默认路径（记住上次的）
    if (!initialFolder.empty()) {
        IShellItem *psiDefault = nullptr;
        HRESULT hrDefault = SHCreateItemFromParsingName(initialFolder.c_str(), nullptr, IID_PPV_ARGS(&psiDefault));
        if (SUCCEEDED(hrDefault) && psiDefault) {
            pfd->SetFolder(psiDefault);
            psiDefault->Release();
        }
    }

    hr = pfd->Show(hParent);
    if (SUCCEEDED(hr)) {
        IShellItem *psi = nullptr;
        hr = pfd->GetResult(&psi);
        if (SUCCEEDED(hr)) {
            wchar_t *path = nullptr;
            hr = psi->GetDisplayName(SIGDN_FILESYSPATH, &path);
            if (SUCCEEDED(hr) && path) {
                outPath = path;
                CoTaskMemFree(path);
            }
            psi->Release();
        }
    }
    pfd->Release();
    return !outPath.empty();
}

// ==========================================================================
// 扫描 .hod 文件（后台线程）
// ==========================================================================

struct ScanParams {
    std::wstring inputDir;
    HWND hWnd;
};

static DWORD WINAPI ScanThreadProc(LPVOID lpParam) {
    ScanParams *params = static_cast<ScanParams *>(lpParam);
    std::wstring inputDir = params->inputDir;
    HWND hWnd = params->hWnd;
    delete params;

    // 清空旧数据
    g_xFiles.clear();
    g_xRelPaths.clear();
    g_hodFiles.clear();
    g_hodRelPaths.clear();
    g_mpdFiles.clear();
    g_mpdRelPaths.clear();
    g_sptFiles.clear();
    g_sptRelPaths.clear();
    g_pngFiles.clear();
    g_pngRelPaths.clear();
    g_aniFiles.clear();
    g_aniRelPaths.clear();

    fs::path inputPath(inputDir);
    if (!fs::is_directory(inputPath)) {
        PostMessageW(hWnd, WM_USER + 1, 0, (LPARAM)L"错误：来源目录不存在");
        return 0;
    }

    int count = 0;
    for (const auto &entry : fs::recursive_directory_iterator(inputPath)) {
        if (!entry.is_regular_file())
            continue;
        std::string ext = entry.path().extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
        if (ext == ".x") {
            g_xFiles.push_back(entry.path());
            fs::path rel = fs::relative(entry.path(), inputPath);
            g_xRelPaths.push_back(rel.string());
            count++;
        } else if (ext == ".hod") {
            g_hodFiles.push_back(entry.path());
            fs::path rel = fs::relative(entry.path(), inputPath);
            g_hodRelPaths.push_back(rel.string());
            count++;
        } else if (ext == ".mpd") {
            g_mpdFiles.push_back(entry.path());
            fs::path rel = fs::relative(entry.path(), inputPath);
            g_mpdRelPaths.push_back(rel.string());
            count++;
        } else if (ext == ".spt") {
            g_sptFiles.push_back(entry.path());
            fs::path rel = fs::relative(entry.path(), inputPath);
            g_sptRelPaths.push_back(rel.string());
            count++;
        } else if (ext == ".png") {
            g_pngFiles.push_back(entry.path());
            fs::path rel = fs::relative(entry.path(), inputPath);
            g_pngRelPaths.push_back(rel.string());
            count++;
        } else if (ext == ".ani") {
            g_aniFiles.push_back(entry.path());
            fs::path rel = fs::relative(entry.path(), inputPath);
            g_aniRelPaths.push_back(rel.string());
            count++;
        }
    }

    // 发消息通知主线程更新列表
    PostMessageW(hWnd, WM_USER + 2, (WPARAM)count, 0);
    return 0;
}

// ==========================================================================
// 转换所有 .hod（后台线程）
// ==========================================================================

static void ConvertAllHOD(const std::wstring &inputDir, const std::wstring &outputDir, HWND hWnd) {
    g_converting = true;
    g_convertProgress = 0;
    g_convertTotal = (int)g_hodFiles.size();
    g_convertSuccess = 0;
    g_convertErrors = 0;

    SendMessageW(g_hProgress, PBM_SETRANGE, 0, MAKELPARAM(0, g_convertTotal));
    SendMessageW(g_hProgress, PBM_SETPOS, 0, 0);

    for (size_t i = 0; i < g_hodFiles.size(); ++i) {
        if (!g_converting)
            break; // 取消

        std::string inputPath = WideToUTF8(g_hodFiles[i].wstring());
        std::string relPath = g_hodRelPaths[i];

        // 输出路径：outputDir + relPath 的目录部分
        fs::path relDir = fs::path(relPath).parent_path();
        fs::path stem = fs::path(relPath).stem();
        fs::path outDir = fs::path(outputDir) / relDir;
        fs::create_directories(outDir);

        // 转换状态更新到列表
        LVITEMW lvi = {};
        lvi.mask = LVIF_TEXT;
        lvi.iItem = (int)i;
        std::wstring convertingText = L"⏳ 转换中...";
        lvi.pszText = const_cast<wchar_t *>(convertingText.c_str());
        lvi.iSubItem = 2;
        ListView_SetItem(g_hList, &lvi);

        AssetTool::HODParser parser;
        if (!parser.ParseFile(inputPath)) {
            std::string errMsg = parser.GetError();
            std::wstring errText = L"❌ " + UTF8ToWide(errMsg);
            lvi.pszText = const_cast<wchar_t *>(errText.c_str());
            ListView_SetItem(g_hList, &lvi);
            // 同步显示错误详情到状态栏
            SetStatusFmt(g_hStatus, L"❌ 解析失败: %s — %s", UTF8ToWide(relPath).c_str(), UTF8ToWide(errMsg).c_str());
            g_convertErrors++;
        } else {
            const auto &hod = parser.GetResult();

            // 输出 JSON（现代骨骼格式）
            std::string jsonPath = (outDir / (stem.string() + ".hod.json")).string();
            bool jsonOk = hod.WriteJSON(jsonPath);

            // 输出 TXT（可读文本）
            std::string txtPath = (outDir / (stem.string() + ".hod.txt")).string();
            bool txtOk = hod.WriteText(txtPath);

            lvi.pszText = const_cast<wchar_t *>(L"✅ 完成");
            ListView_SetItem(g_hList, &lvi);

            if (txtOk)
                g_convertSuccess++;
            else {
                g_convertErrors++;
                SetStatusFmt(g_hStatus, L"❌ 写入输出文件失败: %s", UTF8ToWide(relPath).c_str());
            }
        }

        g_convertProgress = (int)(i + 1);
        SendMessageW(g_hProgress, PBM_SETPOS, g_convertProgress, 0);
        {
            int progress = g_convertProgress.load();
            int total = g_convertTotal.load();
            int succ = g_convertSuccess.load();
            int errs = g_convertErrors.load();
            SetStatusFmt(g_hStatus, L"转换中: %d / %d（成功: %d，失败: %d）", progress, total, succ, errs);
        }
    }

    g_converting = false;
    {
        int succ = g_convertSuccess.load();
        int errs = g_convertErrors.load();
        SetStatusFmt(g_hStatus, L"✅ 转换完成: %d 成功, %d 失败", succ, errs);
    }

    // 恢复按钮状态
    PostMessageW(hWnd, WM_USER + 3, 0, 0);
}

// ==========================================================================
// 转换所有 .ani（后台线程 — ANIParser 拆解：HOD 帧 + Tail 状态机）
// ==========================================================================

static void ConvertAllANI(const std::wstring &inputDir, const std::wstring &outputDir, HWND hWnd) {
    g_converting = true;
    g_convertProgress = 0;
    g_convertTotal = (int)g_aniFiles.size();
    g_convertSuccess = 0;
    g_convertErrors = 0;

    // 调试日志（落盘到输出目录）
    std::ofstream aniLog;
    try {
        aniLog.open((fs::path(outputDir) / L"ani_debug.log").wstring(), std::ios::out | std::ios::trunc);
    } catch (...) {
    }
    if (aniLog.is_open()) {
        aniLog << "=== ANI 拆解调试日志 ===" << "\n";
        aniLog << "输入目录: " << WideToUTF8(inputDir) << "\n";
        aniLog << "输出目录: " << WideToUTF8(outputDir) << "\n";
        aniLog << ".ani 文件数: " << g_aniFiles.size() << "\n\n";
    }

    SendMessageW(g_hProgress, PBM_SETRANGE, 0, MAKELPARAM(0, g_convertTotal));
    SendMessageW(g_hProgress, PBM_SETPOS, 0, 0);

    for (size_t i = 0; i < g_aniFiles.size(); ++i) {
        if (!g_converting)
            break; // 取消

        std::wstring inputPathW = g_aniFiles[i].wstring();
        std::string inputPath = WideToUTF8(inputPathW);
        std::string relPath = g_aniRelPaths[i];

        if (aniLog.is_open()) {
            aniLog << "[" << (i + 1) << "/" << g_aniFiles.size() << "] 输入: " << inputPath << "\n";
        }

        // 输出路径：outputDir + relPath 的目录部分 + ani 文件名（stem）
        fs::path relDir = fs::path(relPath).parent_path();
        fs::path stem = fs::path(relPath).stem();
        fs::path outDir = fs::path(outputDir) / relDir / stem;
        try {
            fs::create_directories(outDir);
        } catch (const std::exception &e) {
            if (aniLog.is_open())
                aniLog << "  ✗ create_directories 失败: " << e.what() << "\n";
            g_convertErrors++;
            continue;
        }

        // 转换状态更新到列表
        LVITEMW lvi = {};
        lvi.mask = LVIF_TEXT;
        lvi.iItem = (int)(g_hodFiles.size() + g_mpdFiles.size() + g_sptFiles.size() + g_xFiles.size() +
                          g_pngFiles.size() + i);
        std::wstring convertingText = L"⏳ 拆解中...";
        lvi.pszText = const_cast<wchar_t *>(convertingText.c_str());
        lvi.iSubItem = 2;
        ListView_SetItem(g_hList, &lvi);

        AssetTool::ANIParser parser;
        bool parseOk = parser.ParseFileW(inputPathW); // 宽路径：避免中文目录打开失败
        if (!parseOk) {
            std::string errMsg = parser.GetError();
            std::wstring errText = L"❌ " + UTF8ToWide(errMsg);
            lvi.pszText = const_cast<wchar_t *>(errText.c_str());
            ListView_SetItem(g_hList, &lvi);
            SetStatusFmt(g_hStatus, L"❌ 解析失败: %s — %s", UTF8ToWide(relPath).c_str(), UTF8ToWide(errMsg).c_str());
            if (aniLog.is_open())
                aniLog << "  ✗ ParseFileW 失败: " << errMsg << "\n";
            g_convertErrors++;
        } else {
            const auto &groups = parser.GetGroups();
            if (aniLog.is_open()) {
                aniLog << "  ✓ 解析成功: " << groups.size() << " 组, "
                       << (parser.GetGroups().empty() ? 0 : (int)parser.GetGroups().size()) << " 组\n";
                if (!parser.GetDiagnostics().empty())
                    aniLog << "    诊断: " << parser.GetDiagnostics() << "\n";
            }
            bool writeOk = parser.WriteOutput(WideToUTF8(outDir.wstring()));
            lvi.pszText = const_cast<wchar_t *>(L"✅ 完成");
            ListView_SetItem(g_hList, &lvi);

            if (writeOk) {
                g_convertSuccess++;
                SetStatusFmt(g_hStatus, L"✅ %s: %d 组动画已拆解", UTF8ToWide(relPath).c_str(),
                             (int)groups.size());
                if (aniLog.is_open())
                    aniLog << "  ✓ WriteOutput 成功 → " << WideToUTF8(outDir.wstring()) << "\n";
            } else {
                g_convertErrors++;
                SetStatusFmt(g_hStatus, L"❌ 写入输出文件失败: %s", UTF8ToWide(relPath).c_str());
                if (aniLog.is_open())
                    aniLog << "  ✗ WriteOutput 失败 → " << WideToUTF8(outDir.wstring()) << "\n";
            }
        }

        g_convertProgress = (int)(i + 1);
        SendMessageW(g_hProgress, PBM_SETPOS, g_convertProgress, 0);
        {
            int progress = g_convertProgress.load();
            int total = g_convertTotal.load();
            int succ = g_convertSuccess.load();
            int errs = g_convertErrors.load();
            SetStatusFmt(g_hStatus, L"转换中: %d / %d（成功: %d，失败: %d）", progress, total, succ, errs);
        }
    }

    if (aniLog.is_open()) {
        aniLog << "\n=== 汇总: 成功 " << g_convertSuccess.load() << ", 失败 " << g_convertErrors.load() << " ===\n";
        aniLog.close();
    }

    g_converting = false;
    {
        int succ = g_convertSuccess.load();
        int errs = g_convertErrors.load();
        SetStatusFmt(g_hStatus, L"✅ 转换完成: %d 成功, %d 失败（日志: ani_debug.log）", succ, errs);
    }

    // 恢复按钮状态
    PostMessageW(hWnd, WM_USER + 3, 0, 0);
}

// ==========================================================================
// 转换所有 .mpd（后台线程）
// ==========================================================================

static void ConvertAllMPD(const std::wstring &inputDir, const std::wstring &outputDir, HWND hWnd) {
    g_converting = true;
    g_convertProgress = 0;
    g_convertTotal = (int)g_mpdFiles.size();
    g_convertSuccess = 0;
    g_convertErrors = 0;

    SendMessageW(g_hProgress, PBM_SETRANGE, 0, MAKELPARAM(0, g_convertTotal));
    SendMessageW(g_hProgress, PBM_SETPOS, 0, 0);

    for (size_t i = 0; i < g_mpdFiles.size(); ++i) {
        if (!g_converting)
            break;

        std::string inputPath = WideToUTF8(g_mpdFiles[i].wstring());
        std::string relPath = g_mpdRelPaths[i];

        fs::path relDir = fs::path(relPath).parent_path();
        fs::path stem = fs::path(relPath).stem();
        fs::path outDir = fs::path(outputDir) / relDir;
        fs::create_directories(outDir);

        LVITEMW lvi = {};
        lvi.mask = LVIF_TEXT;
        // MPD 文件在列表中排在 HOD 文件之后
        int listIdx = (int)(g_hodFiles.size() + i);
        lvi.iItem = listIdx;
        std::wstring convertingText = L"⏳ 转换中...";
        lvi.pszText = const_cast<wchar_t *>(convertingText.c_str());
        lvi.iSubItem = 2;
        ListView_SetItem(g_hList, &lvi);

        AssetTool::MPDParser parser;
        if (!parser.ParseFile(inputPath)) {
            std::string errMsg = parser.GetError();
            std::wstring errText = L"❌ " + UTF8ToWide(errMsg);
            lvi.pszText = const_cast<wchar_t *>(errText.c_str());
            ListView_SetItem(g_hList, &lvi);
            SetStatusFmt(g_hStatus, L"❌ 解析失败: %s — %s", UTF8ToWide(relPath).c_str(), UTF8ToWide(errMsg).c_str());
            g_convertErrors++;
        } else {
            auto mpd = parser.GetResult(); // 改为非 const 以便过滤

            // 自动扫描 MPD 所在目录的 .x 文件，过滤名字表
            fs::path mpdDir = fs::path(inputPath).parent_path();
            std::vector<std::string> xFiles;
            if (fs::exists(mpdDir)) {
                for (auto &entry : fs::directory_iterator(mpdDir)) {
                    if (entry.is_regular_file()) {
                        std::string ext = entry.path().extension().string();
                        if (ext.size() == 2) {
                            ext[0] = static_cast<char>(std::tolower(static_cast<unsigned char>(ext[0])));
                            ext[1] = static_cast<char>(std::tolower(static_cast<unsigned char>(ext[1])));
                        }
                        if (ext == ".x")
                            xFiles.push_back(entry.path().filename().string());
                    }
                }
            }
            if (!xFiles.empty())
                mpd.FilterByExistingFiles(xFiles);

            std::string txtPath = (outDir / (stem.string() + ".mpd.txt")).string();
            bool txtOk = mpd.WriteText(txtPath);

            lvi.pszText = const_cast<wchar_t *>(L"✅ 完成");
            ListView_SetItem(g_hList, &lvi);

            if (txtOk)
                g_convertSuccess++;
            else {
                g_convertErrors++;
                SetStatusFmt(g_hStatus, L"❌ 写入输出文件失败: %s", UTF8ToWide(relPath).c_str());
            }
        }

        g_convertProgress = (int)(i + 1);
        SendMessageW(g_hProgress, PBM_SETPOS, g_convertProgress, 0);
        {
            int progress = g_convertProgress.load();
            int total = g_convertTotal.load();
            int succ = g_convertSuccess.load();
            int errs = g_convertErrors.load();
            SetStatusFmt(g_hStatus, L"转换中: %d / %d（成功: %d，失败: %d）", progress, total, succ, errs);
        }
    }

    g_converting = false;
    {
        int succ = g_convertSuccess.load();
        int errs = g_convertErrors.load();
        SetStatusFmt(g_hStatus, L"✅ 转换完成: %d 成功, %d 失败", succ, errs);
    }

    PostMessageW(hWnd, WM_USER + 3, 0, 0);
}

// ==========================================================================
// 转换所有 .spt（后台线程 — 直接调用管线）
// ==========================================================================

static void ConvertMapScene(const std::wstring &inputDir, const std::wstring &outputDir, HWND hWnd) {
    g_converting = true;
    g_convertProgress = 0;
    g_convertTotal = (int)g_sptFiles.size();
    g_convertSuccess = 0;
    g_convertErrors = 0;

    SendMessageW(g_hProgress, PBM_SETRANGE, 0, MAKELPARAM(0, g_convertTotal));
    SendMessageW(g_hProgress, PBM_SETPOS, 0, 0);

    for (size_t i = 0; i < g_sptFiles.size(); ++i) {
        if (!g_converting)
            break;

        std::string sptPath = WideToUTF8(g_sptFiles[i].wstring());
        std::string relPath = g_sptRelPaths[i];
        fs::path relDir = fs::path(relPath).parent_path();
        std::string mapName = relDir.string();

        // 查找该目录下对应的 .mpd
        fs::path mapDir = fs::path(inputDir) / relDir;
        std::string mpdPath;
        for (const auto &mp : g_mpdFiles) {
            std::string mpStr = WideToUTF8(mp.wstring());
            if (fs::path(mp.wstring()).parent_path() == mapDir) {
                mpdPath = mpStr;
                break;
            }
        }

        if (mpdPath.empty()) {
            SetStatusFmt(g_hStatus, L"⚠ %s: 无 .mpd，跳过", UTF8ToWide(relPath).c_str());
            g_convertErrors++;
            continue;
        }

        // 收集该目录下的所有 .x 文件
        std::vector<std::string> xFiles;
        for (const auto &xf : g_xFiles) {
            std::string xStr = WideToUTF8(xf.wstring());
            if (fs::path(xf.wstring()).parent_path() == mapDir)
                xFiles.push_back(xStr);
        }

        SetStatusFmt(g_hStatus, L"⏳ %s: %zu .x, SPT+MPD 已校验", UTF8ToWide(relPath).c_str(), xFiles.size());

        // 1. 解析 SPT
        AssetTool::ScriptSPTParser sptParser;
        if (!sptParser.ParseFile(sptPath)) {
            SetStatusFmt(g_hStatus, L"❌ %s: SPT 解析失败", UTF8ToWide(relPath).c_str());
            g_convertErrors++;
            continue;
        }
        const auto &sceneData = sptParser.GetResult();

        // 2. 解析 MPD
        AssetTool::MPDParser mpdParser;
        if (!mpdParser.ParseFile(mpdPath)) {
            SetStatusFmt(g_hStatus, L"❌ %s: MPD 解析失败", UTF8ToWide(relPath).c_str());
            g_convertErrors++;
            continue;
        }
        auto mpdData = mpdParser.GetResult(); // copy，用于过滤

        // 用实际 .x 文件过滤 MPD 名字表
        if (!xFiles.empty())
            mpdData.FilterByExistingFiles(xFiles);

        // 3. 创建输出目录
        fs::path outBase = fs::path(outputDir);
        fs::path mapOutDir = outBase / relDir;
        fs::path meshesDir = mapOutDir / "Meshes";
        fs::path matsDir = mapOutDir / "Materials";
        fs::path texDir = mapOutDir / "Textures";
        fs::create_directories(meshesDir);
        fs::create_directories(matsDir);
        fs::create_directories(texDir);

        // 4. 转换所有 .x → .dxmesh + 材质
        struct MeshRef {
            std::string stem;
            std::string matKey;
        };
        std::map<std::string, std::vector<MeshRef>> xMeshMap;
        nlohmann::json allMaterials = nlohmann::json::object();
        nlohmann::json allMeshDeps = nlohmann::json::object();
        nlohmann::json texDeps = nlohmann::json::object();
        int matCount = 0;

        auto processTexture = [&](const std::string &texFile, const fs::path &xDir) -> std::string {
            if (texFile.empty())
                return {};
            fs::path texPath = xDir / texFile;
            if (!fs::exists(texPath)) {
                texPath = mapDir / fs::path(texFile).filename();
                if (!fs::exists(texPath))
                    return {};
            }
            std::string texExt = texPath.extension().string();
            std::transform(texExt.begin(), texExt.end(), texExt.begin(), ::tolower);
            std::string stem = texPath.stem().string();
            std::string ddsName = stem + ".dds";
            fs::path ddsOut = texDir / ddsName;
            if (fs::exists(ddsOut))
                return stem;

            if (texExt == ".dds") {
                auto r = AssetTool::DecryptOrCopyDDS(texPath.string(), ddsOut.string());
                if (r.success)
                    return stem;
            } else if (texExt == ".png" || texExt == ".bmp") {
                auto r = AssetTool::ConvertPNGToDDS(texPath.string(), ddsOut.string());
                if (r.success)
                    return stem;
            }
            return {};
        };

        for (const auto &xPath : xFiles) {
            fs::path xDir = fs::path(xPath).parent_path();
            std::string xname = fs::path(xPath).filename().string();

            AssetTool::XFileParser parser;
            if (!parser.ParseFile(xPath)) {
                SetStatusFmt(g_hStatus, L"⚠ %s: 解析失败", UTF8ToWide(xname).c_str());
                continue;
            }

            const auto &meshes = parser.GetMeshes();
            std::string stem = fs::path(xname).stem().string();
            std::vector<MeshRef> refs;

            for (size_t mi = 0; mi < meshes.size(); ++mi) {
                const auto &mesh = meshes[mi];
                std::string ms = stem;
                if (meshes.size() > 1)
                    ms += "_" + std::to_string(mi);

                mesh.WriteDxMesh((meshesDir / (ms + ".dxmesh")).string());

                auto matDesc = mesh.material.ToMaterialDesc();
                std::string mk = ms + "_mat0";
                nlohmann::json jm;
                jm["shader"] = matDesc.shader;
                jm["params"]["baseColor"] = {matDesc.params.baseColor[0], matDesc.params.baseColor[1],
                                             matDesc.params.baseColor[2], matDesc.params.baseColor[3]};
                jm["params"]["metallic"] = matDesc.params.metallic;
                jm["params"]["roughness"] = matDesc.params.roughness;
                jm["params"]["ao"] = matDesc.params.ao;

                std::string texRef = processTexture(mesh.material.textureFilename, xDir);
                if (!texRef.empty()) {
                    jm["textures"]["baseColor"] = texRef;
                    texDeps[texRef] = "Textures/" + texRef + ".dds";
                }

                std::ofstream matFile((matsDir / (mk + ".mat")).string());
                if (matFile)
                    matFile << jm.dump(2);

                allMaterials[mk] = jm;
                allMeshDeps[ms] = "Meshes/" + ms + ".dxmesh";
                refs.push_back({ms, mk});
                matCount++;
            }
            xMeshMap[xname] = refs;
        }

        // 5. 构建 scene.json
        nlohmann::json scene;
        scene["version"] = 1;
        scene["metadata"]["name"] = mapName;
        scene["metadata"]["description"] = "Converted from " + relPath;
        scene["baseURL"] = "Content/" + mapName;

        scene["environment"]["ambientLight"] = {sceneData.lightR / 255.0f * 0.4f, sceneData.lightG / 255.0f * 0.4f,
                                                sceneData.lightB / 255.0f * 0.4f, 1.0f};

        if (!allMeshDeps.empty())
            scene["dependencies"]["meshes"] = allMeshDeps;
        if (!texDeps.empty())
            scene["dependencies"]["textures"] = texDeps;
        scene["materials"] = allMaterials;

        auto &entities = scene["entities"] = nlohmann::json::array();
        int entityIdx = 0;

        auto addEntity = [&](const std::string &name, float px, float py, float pz, const std::string &meshKey,
                             const std::string &matKey, bool isTransparent, bool isSkybox) {
            nlohmann::json e;
            e["name"] = name;
            auto &c = e["components"];
            c["transform"]["position"] = {px, py, pz};
            c["transform"]["rotation"] = {0, 0, 0, 1};
            c["transform"]["scale"] = {1, 1, 1};
            c["mesh"]["geometry"] = meshKey;
            c["mesh"]["material"] = matKey;
            if (isSkybox) {
                c["skybox"] = nullptr;
                c["transparent"] = nullptr;
            } else if (isTransparent) {
                c["transparent"] = nullptr;
            } else {
                c["opaque"] = nullptr;
            }
            entities.push_back(e);
            entityIdx++;
        };

        auto findRef = [&](const std::string &xfilename) -> MeshRef {
            auto it = xMeshMap.find(xfilename);
            if (it != xMeshMap.end() && !it->second.empty())
                return it->second[0];
            std::string stem = fs::path(xfilename).stem().string();
            for (const auto &[xf, refs] : xMeshMap) {
                if (fs::path(xf).stem().string() == stem && !refs.empty())
                    return refs[0];
            }
            // 前缀匹配：MPD tile stem 是 mesh key 前缀（如 "map00" 匹配 "map00_0"）
            for (const auto &[xf, refs] : xMeshMap) {
                std::string xstem = fs::path(xf).stem().string();
                if (xstem.size() > stem.size() && xstem.substr(0, stem.size()) == stem && xstem[stem.size()] == '_' &&
                    !refs.empty())
                    return refs[0];
            }
            return {};
        };

        // A) MPD 瓦片
        if (!mpdData.tiles.empty()) {
            size_t tileCount = mpdData.tiles.size();
            for (size_t ti = 0; ti < tileCount; ++ti) {
                const auto &t = mpdData.tiles[ti];
                if (t.name.empty())
                    continue;
                std::string xfn = t.name;
                if (!xfn.ends_with(".x"))
                    xfn += ".x";
                auto ref = findRef(xfn);
                // 如果 name 是数字索引，用 name table 还原文件名
                if (ref.stem.empty() && t.tileIndex < mpdData.tileNames.size()) {
                    std::string altName = mpdData.tileNames[t.tileIndex];
                    ref = findRef(altName);
                }
                if (ref.stem.empty())
                    continue;
                addEntity(t.name, t.posX, 0, t.posZ, ref.stem, ref.matKey, false, false);
            }
        } else {
            // 降级：MPD tiles 为空，使用 SPT mapTiles + 网格推算
            if (!sceneData.mapTiles.empty()) {
                size_t tileCount = sceneData.mapTiles.size();
                int gridCols = static_cast<int>(std::ceil(std::sqrt(static_cast<float>(tileCount))));
                float tileSize = 20.0f;
                for (size_t ti = 0; ti < tileCount; ++ti) {
                    const auto &t = sceneData.mapTiles[ti];
                    auto ref = findRef(t.modelFile);
                    if (ref.stem.empty())
                        continue;
                    int row = static_cast<int>(ti) / gridCols;
                    int col = static_cast<int>(ti) % gridCols;
                    float px = col * tileSize + tileSize * 0.5f;
                    float pz = row * tileSize + tileSize * 0.5f;
                    addEntity(fs::path(t.modelFile).stem().string(), px, 0, pz, ref.stem, ref.matKey, false, false);
                }
            }
        }

        // B) 建筑
        for (const auto &b : sceneData.buildings) {
            std::string mf;
            if (b.buildNo >= 0 && (size_t)b.buildNo < sceneData.buildingFiles.size())
                mf = sceneData.buildingFiles[b.buildNo];
            if (mf.empty())
                continue;
            auto ref = findRef(mf);
            if (ref.stem.empty())
                continue;
            addEntity(fs::path(mf).stem().string() + "_" + std::to_string(entityIdx), b.posX, b.posY, b.posZ, ref.stem,
                      ref.matKey, false, false);
        }

        // C) 水面
        if (!sceneData.waterModel.empty()) {
            auto ref = findRef(sceneData.waterModel);
            if (!ref.stem.empty())
                addEntity("water", 0, 0, 0, ref.stem, ref.matKey, true, false);
        }

        // D) 天空盒（顶层属性）
        if (!sceneData.skyModel.empty()) {
            auto ref = findRef(sceneData.skyModel);
            if (!ref.stem.empty()) {
                nlohmann::json sb;
                sb["geometry"] = ref.stem;
                sb["material"] = ref.matKey;
                scene["skybox"] = sb;
            }
        }

        // 写 scene.json
        std::string scenePath = (mapOutDir / (mapName + ".scene.json")).string();
        std::ofstream sf(scenePath);
        if (sf)
            sf << scene.dump(2);

        // 输出 MPD 解析结果用于比对
        std::string mpdTxtPath = (mapOutDir / (mapName + ".mpd.txt")).string();
        mpdData.WriteText(mpdTxtPath);

        // 更新列表状态
        LVITEMW lvi = {};
        lvi.mask = LVIF_TEXT;
        int listIdx = (int)(g_hodFiles.size() + g_mpdFiles.size() + i);
        lvi.iItem = listIdx;
        lvi.pszText = const_cast<wchar_t *>(L"✅ 完成");
        lvi.iSubItem = 2;
        ListView_SetItem(g_hList, &lvi);

        SetStatusFmt(g_hStatus, L"✅ %s: %d 实体", UTF8ToWide(relPath).c_str(), entityIdx);
        g_convertSuccess++;

        g_convertProgress = (int)(i + 1);
        SendMessageW(g_hProgress, PBM_SETPOS, g_convertProgress, 0);
    }

    g_converting = false;
    {
        int succ = g_convertSuccess.load();
        int errs = g_convertErrors.load();
        SetStatusFmt(g_hStatus, L"✅ 地图构建完成: %d 成功, %d 失败", succ, errs);
    }
    PostMessageW(hWnd, WM_USER + 3, 0, 0);
}

// ==========================================================================
// 转换所有 .x 文件（.x → .dxmesh + .mat）
// ==========================================================================

static void ConvertXMeshes(const std::wstring &inputDir, const std::wstring &outputDir, HWND hWnd) {
    g_converting = true;
    g_convertProgress = 0;
    g_convertTotal = (int)g_xFiles.size();
    g_convertSuccess = 0;
    g_convertErrors = 0;

    SendMessageW(g_hProgress, PBM_SETRANGE, 0, MAKELPARAM(0, g_convertTotal));
    SendMessageW(g_hProgress, PBM_SETPOS, 0, 0);

    for (size_t i = 0; i < g_xFiles.size(); ++i) {
        if (!g_converting)
            break;

        std::string inputPath = WideToUTF8(g_xFiles[i].wstring());
        std::string relPath = g_xRelPaths[i];
        fs::path relDir = fs::path(relPath).parent_path();
        fs::path outMeshesDir = fs::path(outputDir) / relDir / "Meshes";
        fs::path outMatsDir = fs::path(outputDir) / relDir / "Materials";
        fs::path outTexDir = fs::path(outputDir) / relDir / "Textures";
        fs::create_directories(outMeshesDir);
        fs::create_directories(outMatsDir);
        fs::create_directories(outTexDir);

        SetStatusFmt(g_hStatus, L"转换网格: %s", UTF8ToWide(relPath).c_str());

        AssetTool::XFileParser parser;
        if (!parser.ParseFile(inputPath)) {
            SetStatusFmt(g_hStatus, L"❌ %s — %s", UTF8ToWide(relPath).c_str(), UTF8ToWide(parser.GetError()).c_str());
            g_convertErrors++;
            g_convertProgress = (int)(i + 1);
            SendMessageW(g_hProgress, PBM_SETPOS, g_convertProgress, 0);
            continue;
        }

        const auto &meshes = parser.GetMeshes();
        std::string stem = fs::path(relPath).stem().string();
        int meshSuccess = 0;

        for (size_t mi = 0; mi < meshes.size(); ++mi) {
            const auto &mesh = meshes[mi];
            std::string ms = stem;
            if (meshes.size() > 1)
                ms += "_" + std::to_string(mi);

            // 输出 .dxmesh
            mesh.WriteDxMesh((outMeshesDir / (ms + ".dxmesh")).string());

            // 输出 .mat 文件
            {
                const auto &xMat = mesh.material;
                auto matDesc = xMat.ToMaterialDesc();
                std::string mk = ms + "_mat0";

                nlohmann::json jm;
                jm["shader"] = matDesc.shader;
                jm["params"]["baseColor"] = {matDesc.params.baseColor[0], matDesc.params.baseColor[1],
                                             matDesc.params.baseColor[2], matDesc.params.baseColor[3]};
                jm["params"]["metallic"] = matDesc.params.metallic;
                jm["params"]["roughness"] = matDesc.params.roughness;
                jm["params"]["ao"] = matDesc.params.ao;
                if (!xMat.textureFilename.empty()) {
                    // 查找同名 .png 并转换为 .dds（跳过损坏的原始 .dds）
                    std::string texFile = xMat.textureFilename;
                    fs::path xDir = fs::path(inputPath).parent_path();
                    std::wstring outName = CopyOrConvertTexture(xDir, texFile, outTexDir);
                    if (!outName.empty()) {
                        std::string outNameA = WideToUTF8(outName);
                        jm["textures"]["baseColor"] = fs::path(outNameA).stem().string();
                    }
                }

                std::ofstream mf((outMatsDir / (mk + ".mat")).string());
                if (mf)
                    mf << jm.dump(2);
            }
            meshSuccess++;
        }

        if (meshSuccess > 0) {
            g_convertSuccess++;
            SetStatusFmt(g_hStatus, L"✅ %s: %d 网格", UTF8ToWide(relPath).c_str(), meshSuccess);
        } else {
            g_convertErrors++;
        }

        g_convertProgress = (int)(i + 1);
        SendMessageW(g_hProgress, PBM_SETPOS, g_convertProgress, 0);
    }

    g_converting = false;
    SetStatusFmt(g_hStatus, L"✅ 网格转换完成: %d 成功, %d 失败", g_convertSuccess.load(), g_convertErrors.load());
    PostMessageW(hWnd, WM_USER + 3, 0, 0);
}

// ==========================================================================
// 转换所有 PNG → DDS
// ==========================================================================

static void ConvertAllPNG(const std::wstring &inputDir, const std::wstring &outputDir, HWND hWnd) {
    g_converting = true;
    g_convertProgress = 0;
    g_convertTotal = (int)g_pngFiles.size();
    g_convertSuccess = 0;
    g_convertErrors = 0;

    SendMessageW(g_hProgress, PBM_SETRANGE, 0, MAKELPARAM(0, g_convertTotal));
    SendMessageW(g_hProgress, PBM_SETPOS, 0, 0);

    for (size_t i = 0; i < g_pngFiles.size(); ++i) {
        if (!g_converting)
            break;

        std::string inputPath = WideToUTF8(g_pngFiles[i].wstring());
        std::string relPath = g_pngRelPaths[i];

        // 输出到同相对路径，扩展名改为 .dds
        fs::path outPath = fs::path(outputDir) / relPath;
        outPath.replace_extension(".dds");

        SetStatusFmt(g_hStatus, L"转换纹理: %s", UTF8ToWide(relPath).c_str());

        auto result = AssetTool::ConvertPNGToDDS(inputPath, outPath.string());
        if (result.success) {
            g_convertSuccess++;
        } else {
            SetStatusFmt(g_hStatus, L"❌ %s — %s", UTF8ToWide(relPath).c_str(), UTF8ToWide(result.error).c_str());
            g_convertErrors++;
        }

        g_convertProgress = (int)(i + 1);
        SendMessageW(g_hProgress, PBM_SETPOS, g_convertProgress, 0);
    }

    g_converting = false;
    SetStatusFmt(g_hStatus, L"✅ 纹理转换完成: %d 成功, %d 失败", g_convertSuccess.load(), g_convertErrors.load());
    PostMessageW(hWnd, WM_USER + 3, 0, 0);
}

// ==========================================================================
// 更新文件列表
// ==========================================================================

static void UpdateFileList(HWND hList, const std::wstring &inputDir) {
    ListView_DeleteAllItems(hList);

    // 显示 .hod 文件
    for (size_t i = 0; i < g_hodFiles.size(); ++i) {
        std::wstring relPath = UTF8ToWide(g_hodRelPaths[i]);
        std::wstring fileName = UTF8ToWide(fs::path(g_hodRelPaths[i]).filename().string());
        std::wstring dirPath = UTF8ToWide(fs::path(g_hodRelPaths[i]).parent_path().string());
        if (dirPath.empty())
            dirPath = L"（根目录）";

        uint64_t fileSize = 0;
        try {
            fileSize = fs::file_size(g_hodFiles[i]);
        } catch (...) {
        }

        wchar_t sizeStr[32];
        if (fileSize < 1024)
            swprintf_s(sizeStr, L"%llu B", fileSize);
        else if (fileSize < 1024 * 1024)
            swprintf_s(sizeStr, L"%.1f KB", fileSize / 1024.0);
        else
            swprintf_s(sizeStr, L"%.1f MB", fileSize / (1024.0 * 1024.0));

        LVITEMW lvi = {};
        lvi.mask = LVIF_TEXT;
        lvi.iItem = (int)i;
        lvi.iSubItem = 0;
        lvi.pszText = const_cast<wchar_t *>(fileName.c_str());
        ListView_InsertItem(g_hList, &lvi);
        lvi.iSubItem = 1;
        lvi.pszText = const_cast<wchar_t *>(relPath.c_str());
        ListView_SetItem(g_hList, &lvi);
        std::wstring status = L"⏳ 等待中 (.hod)";
        lvi.iSubItem = 2;
        lvi.pszText = const_cast<wchar_t *>(status.c_str());
        ListView_SetItem(g_hList, &lvi);
        lvi.iSubItem = 3;
        lvi.pszText = sizeStr;
        ListView_SetItem(g_hList, &lvi);
    }

    // 显示 .mpd 文件（接在 .hod 之后）
    for (size_t i = 0; i < g_mpdFiles.size(); ++i) {
        int idx = (int)(g_hodFiles.size() + i);
        std::wstring relPath = UTF8ToWide(g_mpdRelPaths[i]);
        std::wstring fileName = UTF8ToWide(fs::path(g_mpdRelPaths[i]).filename().string());

        uint64_t fileSize = 0;
        try {
            fileSize = fs::file_size(g_mpdFiles[i]);
        } catch (...) {
        }

        wchar_t sizeStr[32];
        if (fileSize < 1024)
            swprintf_s(sizeStr, L"%llu B", fileSize);
        else if (fileSize < 1024 * 1024)
            swprintf_s(sizeStr, L"%.1f KB", fileSize / 1024.0);
        else
            swprintf_s(sizeStr, L"%.1f MB", fileSize / (1024.0 * 1024.0));

        LVITEMW lvi = {};
        lvi.mask = LVIF_TEXT;
        lvi.iItem = idx;
        lvi.iSubItem = 0;
        lvi.pszText = const_cast<wchar_t *>(fileName.c_str());
        ListView_InsertItem(g_hList, &lvi);
        lvi.iSubItem = 1;
        lvi.pszText = const_cast<wchar_t *>(relPath.c_str());
        ListView_SetItem(g_hList, &lvi);
        std::wstring status = L"⏳ 等待中 (.mpd)";
        lvi.iSubItem = 2;
        lvi.pszText = const_cast<wchar_t *>(status.c_str());
        ListView_SetItem(g_hList, &lvi);
        lvi.iSubItem = 3;
        lvi.pszText = sizeStr;
        ListView_SetItem(g_hList, &lvi);
    }

    // 显示 .spt 文件（接在 .mpd 之后）
    for (size_t i = 0; i < g_sptFiles.size(); ++i) {
        int idx = (int)(g_hodFiles.size() + g_mpdFiles.size() + i);
        std::wstring relPath = UTF8ToWide(g_sptRelPaths[i]);
        std::wstring fileName = UTF8ToWide(fs::path(g_sptRelPaths[i]).filename().string());

        uint64_t fileSize = 0;
        try {
            fileSize = fs::file_size(g_sptFiles[i]);
        } catch (...) {
        }

        wchar_t sizeStr[32];
        if (fileSize < 1024)
            swprintf_s(sizeStr, L"%llu B", fileSize);
        else if (fileSize < 1024 * 1024)
            swprintf_s(sizeStr, L"%.1f KB", fileSize / 1024.0);
        else
            swprintf_s(sizeStr, L"%.1f MB", fileSize / (1024.0 * 1024.0));

        LVITEMW lvi = {};
        lvi.mask = LVIF_TEXT;
        lvi.iItem = idx;
        lvi.iSubItem = 0;
        lvi.pszText = const_cast<wchar_t *>(fileName.c_str());
        ListView_InsertItem(g_hList, &lvi);
        lvi.iSubItem = 1;
        lvi.pszText = const_cast<wchar_t *>(relPath.c_str());
        ListView_SetItem(g_hList, &lvi);
        std::wstring status = L"⏳ 等待中 (.spt → scene)";
        lvi.iSubItem = 2;
        lvi.pszText = const_cast<wchar_t *>(status.c_str());
        ListView_SetItem(g_hList, &lvi);
        lvi.iSubItem = 3;
        lvi.pszText = sizeStr;
        ListView_SetItem(g_hList, &lvi);
    }

    // 显示 .x 文件（接在 .spt 之后）
    for (size_t i = 0; i < g_xFiles.size(); ++i) {
        int idx = (int)(g_hodFiles.size() + g_mpdFiles.size() + g_sptFiles.size() + i);
        std::wstring relPath = UTF8ToWide(g_xRelPaths[i]);
        std::wstring fileName = UTF8ToWide(fs::path(g_xRelPaths[i]).filename().string());

        uint64_t fileSize = 0;
        try {
            fileSize = fs::file_size(g_xFiles[i]);
        } catch (...) {
        }

        wchar_t sizeStr[32];
        if (fileSize < 1024)
            swprintf_s(sizeStr, L"%llu B", fileSize);
        else if (fileSize < 1024 * 1024)
            swprintf_s(sizeStr, L"%.1f KB", fileSize / 1024.0);
        else
            swprintf_s(sizeStr, L"%.1f MB", fileSize / (1024.0 * 1024.0));

        LVITEMW lvi = {};
        lvi.mask = LVIF_TEXT;
        lvi.iItem = idx;
        lvi.iSubItem = 0;
        lvi.pszText = const_cast<wchar_t *>(fileName.c_str());
        ListView_InsertItem(g_hList, &lvi);
        lvi.iSubItem = 1;
        lvi.pszText = const_cast<wchar_t *>(relPath.c_str());
        ListView_SetItem(g_hList, &lvi);
        std::wstring status = L"⏳ 等待中 (.x → .dxmesh)";
        lvi.iSubItem = 2;
        lvi.pszText = const_cast<wchar_t *>(status.c_str());
        ListView_SetItem(g_hList, &lvi);
        lvi.iSubItem = 3;
        lvi.pszText = sizeStr;
        ListView_SetItem(g_hList, &lvi);
    }

    // 显示 .png 文件（接在 .x 之后）
    for (size_t i = 0; i < g_pngFiles.size(); ++i) {
        int idx = (int)(g_hodFiles.size() + g_mpdFiles.size() + g_sptFiles.size() + g_xFiles.size() + i);
        std::wstring relPath = UTF8ToWide(g_pngRelPaths[i]);
        std::wstring fileName = UTF8ToWide(fs::path(g_pngRelPaths[i]).filename().string());

        uint64_t fileSize = 0;
        try {
            fileSize = fs::file_size(g_pngFiles[i]);
        } catch (...) {
        }

        wchar_t sizeStr[32];
        if (fileSize < 1024)
            swprintf_s(sizeStr, L"%llu B", fileSize);
        else if (fileSize < 1024 * 1024)
            swprintf_s(sizeStr, L"%.1f KB", fileSize / 1024.0);
        else
            swprintf_s(sizeStr, L"%.1f MB", fileSize / (1024.0 * 1024.0));

        LVITEMW lvi = {};
        lvi.mask = LVIF_TEXT;
        lvi.iItem = idx;
        lvi.iSubItem = 0;
        lvi.pszText = const_cast<wchar_t *>(fileName.c_str());
        ListView_InsertItem(g_hList, &lvi);
        lvi.iSubItem = 1;
        lvi.pszText = const_cast<wchar_t *>(relPath.c_str());
        ListView_SetItem(g_hList, &lvi);
        std::wstring status = L"⏳ 等待中 (.png → .dds)";
        lvi.iSubItem = 2;
        lvi.pszText = const_cast<wchar_t *>(status.c_str());
        ListView_SetItem(g_hList, &lvi);
        lvi.iSubItem = 3;
        lvi.pszText = sizeStr;
        ListView_SetItem(g_hList, &lvi);
    }

    // 显示 .ani 文件（接在 .png 之后）
    for (size_t i = 0; i < g_aniFiles.size(); ++i) {
        int idx = (int)(g_hodFiles.size() + g_mpdFiles.size() + g_sptFiles.size() + g_xFiles.size() +
                        g_pngFiles.size() + i);
        std::wstring relPath = UTF8ToWide(g_aniRelPaths[i]);
        std::wstring fileName = UTF8ToWide(fs::path(g_aniRelPaths[i]).filename().string());

        uint64_t fileSize = 0;
        try {
            fileSize = fs::file_size(g_aniFiles[i]);
        } catch (...) {
        }

        wchar_t sizeStr[32];
        if (fileSize < 1024)
            swprintf_s(sizeStr, L"%llu B", fileSize);
        else if (fileSize < 1024 * 1024)
            swprintf_s(sizeStr, L"%.1f KB", fileSize / 1024.0);
        else
            swprintf_s(sizeStr, L"%.1f MB", fileSize / (1024.0 * 1024.0));

        LVITEMW lvi = {};
        lvi.mask = LVIF_TEXT;
        lvi.iItem = idx;
        lvi.iSubItem = 0;
        lvi.pszText = const_cast<wchar_t *>(fileName.c_str());
        ListView_InsertItem(g_hList, &lvi);
        lvi.iSubItem = 1;
        lvi.pszText = const_cast<wchar_t *>(relPath.c_str());
        ListView_SetItem(g_hList, &lvi);
        std::wstring status = L"⏳ 等待中 (.ani → 拆解)";
        lvi.iSubItem = 2;
        lvi.pszText = const_cast<wchar_t *>(status.c_str());
        ListView_SetItem(g_hList, &lvi);
        lvi.iSubItem = 3;
        lvi.pszText = sizeStr;
        ListView_SetItem(g_hList, &lvi);
    }

    // 自动调整列宽
    ListView_SetColumnWidth(hList, 0, LVSCW_AUTOSIZE);
    ListView_SetColumnWidth(hList, 1, LVSCW_AUTOSIZE);
    ListView_SetColumnWidth(hList, 2, LVSCW_AUTOSIZE);
    ListView_SetColumnWidth(hList, 3, LVSCW_AUTOSIZE_USEHEADER);
}

// ==========================================================================
// 窗口过程
// ==========================================================================

static LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_CREATE: {
        // ── 字体 ──
        HFONT hFont = CreateFontW(16, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                                  CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Microsoft YaHei UI");

        HFONT hLabelFont = CreateFontW(15, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                                       CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Microsoft YaHei UI");

        int y = 10;
        const int labelW = 80;
        const int editW = 350;
        const int btnW = 100;
        const int btnH = 28;
        const int margin = 12;

        // ── 输入目录 ──
        CreateWindowExW(0, L"STATIC", L"来源目录:", WS_CHILD | WS_VISIBLE, margin, y + 4, labelW, 20, hWnd, nullptr,
                        g_hInst, nullptr);
        if (hLabelFont)
            SendMessageW(GetWindow(hWnd, GW_CHILD), WM_SETFONT, (WPARAM)hLabelFont, TRUE);

        HWND hEditInput =
            CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL | ES_READONLY,
                            margin + labelW, y, editW, 24, hWnd, (HMENU)IDC_MAIN_EDIT_INPUT, g_hInst, nullptr);
        if (hFont)
            SendMessageW(hEditInput, WM_SETFONT, (WPARAM)hFont, TRUE);

        CreateWindowExW(0, L"BUTTON", L"浏览...", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, margin + labelW + editW + 6, y,
                        btnW, btnH, hWnd, (HMENU)IDC_MAIN_BTN_INPUT, g_hInst, nullptr);

        y += 36;

        // ── 输出目录 ──
        CreateWindowExW(0, L"STATIC", L"输出目录:", WS_CHILD | WS_VISIBLE, margin, y + 4, labelW, 20, hWnd, nullptr,
                        g_hInst, nullptr);

        HWND hEditOutput =
            CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL | ES_READONLY,
                            margin + labelW, y, editW, 24, hWnd, (HMENU)IDC_MAIN_EDIT_OUTPUT, g_hInst, nullptr);
        if (hFont)
            SendMessageW(hEditOutput, WM_SETFONT, (WPARAM)hFont, TRUE);

        CreateWindowExW(0, L"BUTTON", L"浏览...", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, margin + labelW + editW + 6, y,
                        btnW, btnH, hWnd, (HMENU)IDC_MAIN_BTN_OUTPUT, g_hInst, nullptr);

        y += 36;

        // ── 操作按钮 ──
        g_hBtnScan =
            CreateWindowExW(0, L"BUTTON", L"🔍 扫描（.x/.hod/.spt/.png/.ani）", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                            margin, y, 190, 32, hWnd, (HMENU)IDC_MAIN_BTN_SCAN, g_hInst, nullptr);
        if (hFont)
            SendMessageW(g_hBtnScan, WM_SETFONT, (WPARAM)hFont, TRUE);

        // 类型独立转换按钮
        const int convertBtnW = 120;
        g_hBtnConvertHOD =
            CreateWindowExW(0, L"BUTTON", L"HOD → JSON/TXT", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | WS_DISABLED,
                            margin + 190, y, convertBtnW, 32, hWnd, (HMENU)IDC_MAIN_BTN_CONVERT_HOD, g_hInst, nullptr);
        if (hFont)
            SendMessageW(g_hBtnConvertHOD, WM_SETFONT, (WPARAM)hFont, TRUE);

        g_hBtnConvertMesh =
            CreateWindowExW(0, L"BUTTON", L"Mesh → .dxmesh", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | WS_DISABLED,
                            margin + 190 + convertBtnW + 6, y, convertBtnW, 32, hWnd, (HMENU)IDC_MAIN_BTN_CONVERT_MESH,
                            g_hInst, nullptr);
        if (hFont)
            SendMessageW(g_hBtnConvertMesh, WM_SETFONT, (WPARAM)hFont, TRUE);

        g_hBtnMapBuild =
            CreateWindowExW(0, L"BUTTON", L"Map Build", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | WS_DISABLED,
                            margin + 190 + (convertBtnW + 6) * 2, y, convertBtnW, 32, hWnd,
                            (HMENU)IDC_MAIN_BTN_MAP_BUILD, g_hInst, nullptr);
        if (hFont)
            SendMessageW(g_hBtnMapBuild, WM_SETFONT, (WPARAM)hFont, TRUE);

        g_hBtnConvertPNG =
            CreateWindowExW(0, L"BUTTON", L"PNG → DDS", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | WS_DISABLED,
                            margin + 190 + (convertBtnW + 6) * 3, y, convertBtnW, 32, hWnd,
                            (HMENU)IDC_MAIN_BTN_CONVERT_PNG, g_hInst, nullptr);
        if (hFont)
            SendMessageW(g_hBtnConvertPNG, WM_SETFONT, (WPARAM)hFont, TRUE);

        // "导入机体"（ANI → 引擎资产）+ "ANI → FBX"（唯一 FBX 导出）+ "ANI 拆解"（同一行）
        y += 38;
        g_hBtnImportRobot = CreateWindowExW(0, L"BUTTON", L"🤖 导入机体",
                                            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | WS_DISABLED, margin, y, 200, 32,
                                            hWnd, (HMENU)IDC_MAIN_BTN_IMPORT_ROBOT, g_hInst, nullptr);
        if (hFont)
            SendMessageW(g_hBtnImportRobot, WM_SETFONT, (WPARAM)hFont, TRUE);

        // "ANI → FBX"：唯一 FBX 导出选项（动画全量 + 材质 + Emissive 归一；x 已废弃不再导出）
        g_hBtnImportANI =
            CreateWindowExW(0, L"BUTTON", L"ANI → FBX",
                            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | WS_DISABLED, margin + 206, y, 120, 32, hWnd,
                            (HMENU)IDC_MAIN_BTN_IMPORT_ANI, g_hInst, nullptr);
        if (hFont)
            SendMessageW(g_hBtnImportANI, WM_SETFONT, (WPARAM)hFont, TRUE);

        // "ANI 拆解" 按钮（跟在 ANI → FBX 后面）
        g_hBtnConvertANI =
            CreateWindowExW(0, L"BUTTON", L"ANI 拆解",
                            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | WS_DISABLED, margin + 332, y, 120, 32, hWnd,
                            (HMENU)IDC_MAIN_BTN_CONVERT_ANI, g_hInst, nullptr);
        if (hFont)
            SendMessageW(g_hBtnConvertANI, WM_SETFONT, (WPARAM)hFont, TRUE);

        y += 42;

        // ── 列表视图 ──
        RECT rcClient;
        GetClientRect(hWnd, &rcClient);
        int listH = rcClient.bottom - y - 80;

        g_hList = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"",
                                  WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_SINGLESEL | LVS_NOSORTHEADER, margin, y,
                                  rcClient.right - margin * 2, listH, hWnd, (HMENU)IDC_MAIN_LIST, g_hInst, nullptr);
        if (hFont)
            SendMessageW(g_hList, WM_SETFONT, (WPARAM)hFont, TRUE);

        // 设置列
        LVCOLUMNW lvc = {};
        lvc.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
        lvc.cx = 180;
        lvc.pszText = const_cast<wchar_t *>(L"文件名");
        ListView_InsertColumn(g_hList, 0, &lvc);

        lvc.cx = 300;
        lvc.pszText = const_cast<wchar_t *>(L"相对路径");
        ListView_InsertColumn(g_hList, 1, &lvc);

        lvc.cx = 120;
        lvc.pszText = const_cast<wchar_t *>(L"状态");
        ListView_InsertColumn(g_hList, 2, &lvc);

        lvc.cx = 80;
        lvc.pszText = const_cast<wchar_t *>(L"大小");
        ListView_InsertColumn(g_hList, 3, &lvc);

        // 设置完整行选择 + 网格线
        ListView_SetExtendedListViewStyle(g_hList, LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES);

        y += listH + 6;

        // ── 进度条 ──
        g_hProgress =
            CreateWindowExW(0, PROGRESS_CLASSW, nullptr, WS_CHILD | WS_VISIBLE | PBS_SMOOTH, margin, y,
                            rcClient.right - margin * 2, 20, hWnd, (HMENU)IDC_MAIN_PROGRESS, g_hInst, nullptr);

        y += 26;

        // ── 状态栏 ──
        g_hStatus = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"就绪。选择来源目录后点击「扫描」。",
                                    WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL | ES_READONLY, margin, y,
                                    rcClient.right - margin * 2, 22, hWnd, (HMENU)IDC_MAIN_STATUS, g_hInst, nullptr);
        if (hFont)
            SendMessageW(g_hStatus, WM_SETFONT, (WPARAM)hFont, TRUE);

        // 释放字体
        if (hFont)
            DeleteObject(hFont);
        if (hLabelFont)
            DeleteObject(hLabelFont);

        // 初始化 COM
        CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        InitCommonControls();

        // 支持拖放
        DragAcceptFiles(hWnd, TRUE);

        break;
    }

    case WM_SIZE: {
        RECT rc;
        GetClientRect(hWnd, &rc);

        // 只调整列表和进度条、状态栏的宽度
        const int margin = 12;
        int listY = 42 + 36 + 42 + 38; // labels + btn row 1 + btn row 2
        int listH = rc.bottom - listY - 80;

        if (g_hList) {
            SetWindowPos(g_hList, nullptr, margin, listY, rc.right - margin * 2, listH, SWP_NOZORDER);
        }
        if (g_hProgress) {
            SetWindowPos(g_hProgress, nullptr, margin, listY + listH + 6, rc.right - margin * 2, 20, SWP_NOZORDER);
        }
        if (g_hStatus) {
            SetWindowPos(g_hStatus, nullptr, margin, listY + listH + 32, rc.right - margin * 2, 22, SWP_NOZORDER);
        }
        break;
    }

    case WM_COMMAND: {
        int id = LOWORD(wParam);

        if (id == IDC_MAIN_BTN_INPUT) {
            std::wstring path;
            if (PickFolderDialog(hWnd, path, L"选择来源目录（UKW 游戏文件夹）", g_lastInputDir)) {
                g_lastInputDir = path;
                SetDlgItemTextW(hWnd, IDC_MAIN_EDIT_INPUT, path.c_str());
                SetStatusFmt(g_hStatus, L"来源目录: %s", path.c_str());
            }
            return 0;
        }

        if (id == IDC_MAIN_BTN_OUTPUT) {
            std::wstring path;
            if (PickFolderDialog(hWnd, path, L"选择输出目录", g_lastOutputDir)) {
                g_lastOutputDir = path;
                SetDlgItemTextW(hWnd, IDC_MAIN_EDIT_OUTPUT, path.c_str());
            }
            return 0;
        }

        if (id == IDC_MAIN_BTN_SCAN) {
            if (g_converting) {
                SetStatusText(g_hStatus, L"正在转换中，请等待...");
                return 0;
            }

            wchar_t inputDir[MAX_PATH] = {};
            GetDlgItemTextW(hWnd, IDC_MAIN_EDIT_INPUT, inputDir, MAX_PATH);
            if (inputDir[0] == 0) {
                SetStatusText(g_hStatus, L"请先选择来源目录");
                MessageBoxW(hWnd, L"请先选择来源目录。", L"提示", MB_OK | MB_ICONINFORMATION);
                return 0;
            }

            SetStatusFmt(g_hStatus, L"正在扫描 %s 中的资产文件...", inputDir);
            EnableWindow(g_hBtnScan, FALSE);
            EnableWindow(g_hBtnConvertMesh, FALSE);
            EnableWindow(g_hBtnConvertHOD, FALSE);
            EnableWindow(g_hBtnMapBuild, FALSE);
            EnableWindow(g_hBtnConvertPNG, FALSE);

            ScanParams *params = new ScanParams{inputDir, hWnd};
            CreateThread(nullptr, 0, ScanThreadProc, params, 0, nullptr);
            return 0;
        }

        // ── 类型独立转换按钮 ──
        // Mesh (.x → .dxmesh)
        if (id == IDC_MAIN_BTN_CONVERT_MESH) {
            if (g_converting || g_xFiles.empty())
                return 0;
            wchar_t inputDir[MAX_PATH] = {}, outputDir[MAX_PATH] = {};
            GetDlgItemTextW(hWnd, IDC_MAIN_EDIT_INPUT, inputDir, MAX_PATH);
            GetDlgItemTextW(hWnd, IDC_MAIN_EDIT_OUTPUT, outputDir, MAX_PATH);
            if (outputDir[0] == 0) {
                SetStatusText(g_hStatus, L"请先选择输出目录。");
                MessageBoxW(hWnd, L"请先选择输出目录。", L"提示", MB_OK | MB_ICONINFORMATION);
                return 0;
            }
            EnableWindow(g_hBtnScan, FALSE);
            EnableWindow(g_hBtnConvertMesh, FALSE);
            EnableWindow(g_hBtnConvertHOD, FALSE);
            EnableWindow(g_hBtnMapBuild, FALSE);
            EnableWindow(g_hBtnConvertPNG, FALSE);
            EnableWindow(g_hBtnConvertANI, FALSE);
            std::wstring inDir = inputDir, outDir = outputDir;
            std::thread t([inDir, outDir, hWnd]() { ConvertXMeshes(inDir, outDir, hWnd); });
            t.detach();
            return 0;
        }

        // HOD (.hod → JSON/TXT)
        if (id == IDC_MAIN_BTN_CONVERT_HOD) {
            if (g_converting || g_hodFiles.empty())
                return 0;
            wchar_t inputDir[MAX_PATH] = {}, outputDir[MAX_PATH] = {};
            GetDlgItemTextW(hWnd, IDC_MAIN_EDIT_INPUT, inputDir, MAX_PATH);
            GetDlgItemTextW(hWnd, IDC_MAIN_EDIT_OUTPUT, outputDir, MAX_PATH);
            if (outputDir[0] == 0) {
                SetStatusText(g_hStatus, L"请先选择输出目录。");
                MessageBoxW(hWnd, L"请先选择输出目录。", L"提示", MB_OK | MB_ICONINFORMATION);
                return 0;
            }
            EnableWindow(g_hBtnScan, FALSE);
            EnableWindow(g_hBtnConvertMesh, FALSE);
            EnableWindow(g_hBtnConvertHOD, FALSE);
            EnableWindow(g_hBtnMapBuild, FALSE);
            EnableWindow(g_hBtnConvertPNG, FALSE);
            EnableWindow(g_hBtnConvertANI, FALSE);
            std::wstring inDir = inputDir, outDir = outputDir;
            std::thread t([inDir, outDir, hWnd]() { ConvertAllHOD(inDir, outDir, hWnd); });
            t.detach();
            return 0;
        }

        // Map Build（SPT + MPD + 全部 .x → scene.json）
        if (id == IDC_MAIN_BTN_MAP_BUILD) {
            if (g_converting || g_sptFiles.empty())
                return 0;
            wchar_t inputDir[MAX_PATH] = {}, outputDir[MAX_PATH] = {};
            GetDlgItemTextW(hWnd, IDC_MAIN_EDIT_INPUT, inputDir, MAX_PATH);
            GetDlgItemTextW(hWnd, IDC_MAIN_EDIT_OUTPUT, outputDir, MAX_PATH);
            if (outputDir[0] == 0) {
                SetStatusText(g_hStatus, L"请先选择输出目录。");
                MessageBoxW(hWnd, L"请先选择输出目录。", L"提示", MB_OK | MB_ICONINFORMATION);
                return 0;
            }
            EnableWindow(g_hBtnScan, FALSE);
            EnableWindow(g_hBtnConvertMesh, FALSE);
            EnableWindow(g_hBtnConvertHOD, FALSE);
            EnableWindow(g_hBtnMapBuild, FALSE);
            EnableWindow(g_hBtnConvertPNG, FALSE);
            EnableWindow(g_hBtnConvertANI, FALSE);
            std::wstring inDir = inputDir, outDir = outputDir;
            std::thread t([inDir, outDir, hWnd]() { ConvertMapScene(inDir, outDir, hWnd); });
            t.detach();
            return 0;
        }

        // PNG → DDS
        if (id == IDC_MAIN_BTN_CONVERT_PNG) {
            if (g_converting || g_pngFiles.empty())
                return 0;
            wchar_t inputDir[MAX_PATH] = {}, outputDir[MAX_PATH] = {};
            GetDlgItemTextW(hWnd, IDC_MAIN_EDIT_INPUT, inputDir, MAX_PATH);
            GetDlgItemTextW(hWnd, IDC_MAIN_EDIT_OUTPUT, outputDir, MAX_PATH);
            if (outputDir[0] == 0) {
                SetStatusText(g_hStatus, L"请先选择输出目录。");
                MessageBoxW(hWnd, L"请先选择输出目录。", L"提示", MB_OK | MB_ICONINFORMATION);
                return 0;
            }
            EnableWindow(g_hBtnScan, FALSE);
            EnableWindow(g_hBtnConvertMesh, FALSE);
            EnableWindow(g_hBtnConvertHOD, FALSE);
            EnableWindow(g_hBtnMapBuild, FALSE);
            EnableWindow(g_hBtnConvertPNG, FALSE);
            EnableWindow(g_hBtnConvertANI, FALSE);
            std::wstring inDir = inputDir, outDir = outputDir;
            std::thread t([inDir, outDir, hWnd]() { ConvertAllPNG(inDir, outDir, hWnd); });
            t.detach();
            return 0;
        }

        // 导入机体（ANI 驱动 → 引擎资产：dxmesh/hod.json/scene.json/.bone/.mat，不含 FBX）
        if (id == IDC_MAIN_BTN_IMPORT_ROBOT) {
            if (g_converting || g_aniFiles.empty())
                return 0;
            wchar_t outputDir[MAX_PATH] = {};
            GetDlgItemTextW(hWnd, IDC_MAIN_EDIT_OUTPUT, outputDir, MAX_PATH);
            if (outputDir[0] == 0) {
                SetStatusText(g_hStatus, L"请先选择输出目录。");
                MessageBoxW(hWnd, L"请先选择输出目录。", L"提示", MB_OK | MB_ICONINFORMATION);
                return 0;
            }

            // 禁用所有按钮
            EnableWindow(g_hBtnScan, FALSE);
            EnableWindow(g_hBtnConvertMesh, FALSE);
            EnableWindow(g_hBtnConvertHOD, FALSE);
            EnableWindow(g_hBtnMapBuild, FALSE);
            EnableWindow(g_hBtnConvertPNG, FALSE);
            EnableWindow(g_hBtnImportRobot, FALSE);
            EnableWindow(g_hBtnConvertANI, FALSE);
            EnableWindow(g_hBtnImportANI, FALSE);

            // 读取输入目录（用于计算相对路径）
            wchar_t inputDir[MAX_PATH] = {};
            GetDlgItemTextW(hWnd, IDC_MAIN_EDIT_INPUT, inputDir, MAX_PATH);

            // 对每个 .ani 文件执行 MergeFromANI（骨骼信息只从 ANI 首帧提取，不依赖同目录 Robo.hod）
            std::wstring outDir = outputDir;
            std::wstring inDir = inputDir;
            std::vector<fs::path> aniCopy = g_aniFiles;
            std::thread t([aniCopy, inDir, outDir, hWnd]() {
                g_converting = true;
                g_convertTotal = static_cast<int>(aniCopy.size());
                g_convertProgress = 0;
                g_convertSuccess = 0;
                g_convertErrors = 0;
                int idx = 0;
                for (const auto &aniPath : aniCopy) {
                    if (!g_converting)
                        break;
                    idx++;
                    g_convertProgress = idx;
                    std::string aniA = WideToUTF8(aniPath.wstring());
                    std::string outA = WideToUTF8(outDir);
                    std::string inA = WideToUTF8(inDir);
                    fs::path relPath = fs::relative(aniPath, inA);
                    std::string robotOutDir = outA + "/" + relPath.parent_path().string();
                    fs::create_directories(robotOutDir);
                    SetStatusFmt(g_hStatus, L"[%d/%d] 导入: %s", idx, (int)aniCopy.size(), aniPath.filename().c_str());
                    AssetTool::RobotMergeOptions opts;
                    opts.lrSwap = true; // LR 交换固定开启（骨骼树正确性优先，GUI 不再暴露选项）
                    try {
                        auto result = AssetTool::RobotMerger::MergeFromANI(aniA, robotOutDir, opts);
                        if (result.success)
                            g_convertSuccess++;
                        else {
                            g_convertErrors++;
                            SetStatusFmt(g_hStatus, L"❌ 失败: %s", UTF8ToWide(result.error).c_str());
                        }
                    } catch (const std::exception &e) {
                        g_convertErrors++;
                        SetStatusFmt(g_hStatus, L"❌ 异常: %s", UTF8ToWide(e.what()).c_str());
                    }
                }
                g_converting = false;
                PostMessageW(hWnd, WM_USER + 3, 0, 0);
                SetStatusFmt(g_hStatus, L"导入完成: %d 成功, %d 失败 (%d 机体)", (int)g_convertSuccess,
                             (int)g_convertErrors, (int)aniCopy.size());
            });
            t.detach();
            return 0;
        }

        // ANI → FBX（唯一 FBX 导出：动画全量 + 材质子网格 + Emissive 归一；不含引擎资产）
        if (id == IDC_MAIN_BTN_IMPORT_ANI) {
            if (g_converting || g_aniFiles.empty())
                return 0;
            wchar_t outputDir[MAX_PATH] = {};
            GetDlgItemTextW(hWnd, IDC_MAIN_EDIT_OUTPUT, outputDir, MAX_PATH);
            if (outputDir[0] == 0) {
                SetStatusText(g_hStatus, L"请先选择输出目录。");
                MessageBoxW(hWnd, L"请先选择输出目录。", L"提示", MB_OK | MB_ICONINFORMATION);
                return 0;
            }

            // 禁用所有按钮
            EnableWindow(g_hBtnScan, FALSE);
            EnableWindow(g_hBtnConvertMesh, FALSE);
            EnableWindow(g_hBtnConvertHOD, FALSE);
            EnableWindow(g_hBtnMapBuild, FALSE);
            EnableWindow(g_hBtnConvertPNG, FALSE);
            EnableWindow(g_hBtnImportRobot, FALSE);
            EnableWindow(g_hBtnConvertANI, FALSE);
            EnableWindow(g_hBtnImportANI, FALSE);

            // 读取输入目录（用于计算相对路径）
            wchar_t inputDir[MAX_PATH] = {};
            GetDlgItemTextW(hWnd, IDC_MAIN_EDIT_INPUT, inputDir, MAX_PATH);

            std::wstring outDir = outputDir;
            std::wstring inDir = inputDir;
            std::vector<fs::path> aniCopy = g_aniFiles;
            std::thread t([aniCopy, inDir, outDir, hWnd]() {
                g_converting = true;
                g_convertTotal = static_cast<int>(aniCopy.size());
                g_convertProgress = 0;
                g_convertSuccess = 0;
                g_convertErrors = 0;
                int idx = 0;
                for (const auto &aniPath : aniCopy) {
                    if (!g_converting)
                        break;
                    idx++;
                    g_convertProgress = idx;
                    std::string aniA = WideToUTF8(aniPath.wstring());
                    std::string outA = WideToUTF8(outDir);
                    std::string inA = WideToUTF8(inDir);
                    fs::path relPath = fs::relative(aniPath, inA);
                    std::string robotOutDir = outA + "/" + relPath.parent_path().string();
                    fs::create_directories(robotOutDir);
                    SetStatusFmt(g_hStatus, L"[%d/%d] ANI → FBX: %s", idx, (int)aniCopy.size(), aniPath.filename().c_str());
                    // 骨骼信息只从 ANI 首帧提取（不依赖同目录 Robo.hod）；输出 {stem}_anim.fbx
                    std::string animStem = aniPath.stem().string();
                    if (animStem.empty())
                        animStem = "Robo";
                    try {
                        auto animResult = AssetTool::RobotMerger::ExportAnimationsFBX(aniA, robotOutDir, animStem);
                        if (animResult.success)
                            g_convertSuccess++;
                        else {
                            g_convertErrors++;
                            SetStatusFmt(g_hStatus, L"❌ FBX 导出失败: %s", UTF8ToWide(animResult.error).c_str());
                        }
                    } catch (const std::exception &e) {
                        g_convertErrors++;
                        SetStatusFmt(g_hStatus, L"❌ 异常: %s", UTF8ToWide(e.what()).c_str());
                    }
                }
                g_converting = false;
                PostMessageW(hWnd, WM_USER + 3, 0, 0);
                SetStatusFmt(g_hStatus, L"ANI → FBX 完成: %d 成功, %d 失败 (%d 动画)", (int)g_convertSuccess,
                             (int)g_convertErrors, (int)aniCopy.size());
            });
            t.detach();
            return 0;
        }

        // ANI 拆解（.ani → HOD 帧 + Tail 状态机）
        if (id == IDC_MAIN_BTN_CONVERT_ANI) {
            if (g_converting || g_aniFiles.empty())
                return 0;
            wchar_t outputDir[MAX_PATH] = {};
            GetDlgItemTextW(hWnd, IDC_MAIN_EDIT_OUTPUT, outputDir, MAX_PATH);
            if (outputDir[0] == 0) {
                SetStatusText(g_hStatus, L"请先选择输出目录。");
                MessageBoxW(hWnd, L"请先选择输出目录。", L"提示", MB_OK | MB_ICONINFORMATION);
                return 0;
            }

            // 禁用所有按钮
            EnableWindow(g_hBtnScan, FALSE);
            EnableWindow(g_hBtnConvertMesh, FALSE);
            EnableWindow(g_hBtnConvertHOD, FALSE);
            EnableWindow(g_hBtnMapBuild, FALSE);
            EnableWindow(g_hBtnConvertPNG, FALSE);
            EnableWindow(g_hBtnImportRobot, FALSE);
            EnableWindow(g_hBtnConvertANI, FALSE);
            EnableWindow(g_hBtnImportANI, FALSE);

            // 读取输入目录（用于计算相对路径）
            wchar_t inputDir[MAX_PATH] = {};
            GetDlgItemTextW(hWnd, IDC_MAIN_EDIT_INPUT, inputDir, MAX_PATH);

            std::wstring outDir = outputDir;
            std::wstring inDir = inputDir;
            std::thread t([inDir, outDir, hWnd]() { ConvertAllANI(inDir, outDir, hWnd); });
            t.detach();
            return 0;
        }

        if (id == IDC_MAIN_BTN_OPEN_OUTPUT) {
            wchar_t outputDir[MAX_PATH] = {};
            GetDlgItemTextW(hWnd, IDC_MAIN_EDIT_OUTPUT, outputDir, MAX_PATH);
            if (outputDir[0] != 0) {
                ShellExecuteW(hWnd, L"open", outputDir, nullptr, nullptr, SW_SHOW);
            }
            return 0;
        }

        break;
    }

    case WM_USER + 1: {
        // Scan error
        const wchar_t *errMsg = reinterpret_cast<const wchar_t *>(lParam);
        SetStatusText(g_hStatus, errMsg);
        EnableWindow(g_hBtnScan, TRUE);
        break;
    }

    case WM_USER + 2: {
        // Scan complete
        int count = (int)wParam;
        wchar_t inputDir[MAX_PATH] = {};
        GetDlgItemTextW(hWnd, IDC_MAIN_EDIT_INPUT, inputDir, MAX_PATH);

        UpdateFileList(g_hList, inputDir);
        int xCount = (int)g_xFiles.size();
        int hodCount = (int)g_hodFiles.size();
        int mpdCount = (int)g_mpdFiles.size();
        int sptCount = (int)g_sptFiles.size();
        int pngCount = (int)g_pngFiles.size();
        int aniCount = (int)g_aniFiles.size();
        if (count > 0)
            SetStatusFmt(g_hStatus, L"找到 %d 个文件（.x=%d, .hod=%d, .mpd=%d, .spt=%d, .png=%d, .ani=%d）。", count,
                         xCount, hodCount, mpdCount, sptCount, pngCount, aniCount);
        else
            SetStatusText(g_hStatus, L"未找到资产文件。请确认目录正确。");
        EnableWindow(g_hBtnScan, TRUE);
        EnableWindow(g_hBtnConvertMesh, xCount > 0 ? TRUE : FALSE);
        EnableWindow(g_hBtnConvertHOD, hodCount > 0 ? TRUE : FALSE);
        EnableWindow(g_hBtnMapBuild, sptCount > 0 && mpdCount > 0 ? TRUE : FALSE);
        EnableWindow(g_hBtnConvertPNG, pngCount > 0 ? TRUE : FALSE);
        EnableWindow(g_hBtnImportRobot, aniCount > 0 ? TRUE : FALSE);
        EnableWindow(g_hBtnConvertANI, aniCount > 0 ? TRUE : FALSE);
        EnableWindow(g_hBtnImportANI, aniCount > 0 ? TRUE : FALSE);
        break;
    }

    case WM_USER + 3: {
        // Convert complete (button re-enable)
        EnableWindow(g_hBtnScan, TRUE);
        EnableWindow(g_hBtnConvertMesh, !g_xFiles.empty());
        EnableWindow(g_hBtnConvertHOD, !g_hodFiles.empty());
        EnableWindow(g_hBtnMapBuild, !g_sptFiles.empty());
        EnableWindow(g_hBtnConvertPNG, !g_pngFiles.empty());
        EnableWindow(g_hBtnImportRobot, !g_aniFiles.empty());
        EnableWindow(g_hBtnConvertANI, !g_aniFiles.empty());
        EnableWindow(g_hBtnImportANI, !g_aniFiles.empty());
        break;
    }

    case WM_DROPFILES: {
        // 支持拖放目录到窗口
        HDROP hDrop = (HDROP)wParam;
        wchar_t path[MAX_PATH];
        DragQueryFileW(hDrop, 0, path, MAX_PATH);

        if (PathIsDirectoryW(path)) {
            SetDlgItemTextW(hWnd, IDC_MAIN_EDIT_INPUT, path);

            // 自动扫描
            if (g_hodFiles.empty()) {
                SetStatusFmt(g_hStatus, L"来源目录: %s", path);
                ScanParams *params = new ScanParams{path, hWnd};
                CreateThread(nullptr, 0, ScanThreadProc, params, 0, nullptr);
            }
        }
        DragFinish(hDrop);
        break;
    }

    case WM_CLOSE: {
        if (g_converting) {
            if (MessageBoxW(hWnd, L"正在转换中，确定要退出吗？", L"确认", MB_YESNO | MB_ICONQUESTION) == IDNO) {
                return 0;
            }
            g_converting = false;
        }
        DestroyWindow(hWnd);
        break;
    }

    case WM_DESTROY: {
        CoUninitialize();
        PostQuitMessage(0);
        break;
    }

    default:
        return DefWindowProcW(hWnd, msg, wParam, lParam);
    }
    return 0;
}

// ==========================================================================
// WinMain — GUI 入口
// ==========================================================================

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE, LPSTR, int nCmdShow) {
    g_hInst = hInstance;

    // 注册窗口类
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(WNDCLASSEXW);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName = L"AssetToolClass";

    if (!RegisterClassExW(&wc)) {
        MessageBoxW(nullptr, L"窗口注册失败！", L"错误", MB_OK | MB_ICONERROR);
        return 1;
    }

    // 计算窗口尺寸
    const int width = 700;
    const int height = 550;

    RECT rc = {0, 0, width, height};
    AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, FALSE);

    int screenW = GetSystemMetrics(SM_CXSCREEN);
    int screenH = GetSystemMetrics(SM_CYSCREEN);
    int x = (screenW - (rc.right - rc.left)) / 2;
    int y = (screenH - (rc.bottom - rc.top)) / 2;

    // 创建窗口
    g_hWnd = CreateWindowExW(0, L"AssetToolClass", L"AssetTool — .hod 骨骼转换工具", WS_OVERLAPPEDWINDOW, x, y,
                             rc.right - rc.left, rc.bottom - rc.top, nullptr, nullptr, hInstance, nullptr);

    if (!g_hWnd) {
        MessageBoxW(nullptr, L"窗口创建失败！", L"错误", MB_OK | MB_ICONERROR);
        return 1;
    }

    ShowWindow(g_hWnd, nCmdShow);
    UpdateWindow(g_hWnd);

    // 消息循环
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    return (int)msg.wParam;
}
