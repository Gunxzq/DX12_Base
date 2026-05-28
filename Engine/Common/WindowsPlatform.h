// ========== WindowsPlatform.h ==========
#pragma once

// ---------- Windows 优化 ----------
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

// 防止 Windows.h 定义 min/max 宏，与 std::min/std::max 和第三方库冲突
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <Windowsx.h>
#include <windows.h>

// ---------- 取消定义 Windows API 宏 ----------
#ifdef CreateWindow
#undef CreateWindow
#endif
#ifdef GetWindowLong
#undef GetWindowLong
#endif
#ifdef GetWindowText
#undef GetWindowText
#endif
#ifdef SetWindowText
#undef SetWindowText
#endif

// ---------- C 运行时 ----------
#include <malloc.h>
#include <stdlib.h>

// ---------- 平台特定工具 ----------
#ifdef _WIN32
#include <cstdlib> // for _byteswap_ulong if needed
#endif
