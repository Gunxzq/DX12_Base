// ========== WindowsPlatform.h ==========
#pragma once

// ---------- Windows 优化 ----------
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

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

// ---------- C++ 标准库（高频） ----------
#include <cstdio>
#include <cstdint>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <vector>
#include <functional>
#include <chrono>
#include <sstream>
#include <stdexcept>
#include <unordered_map>
#include <filesystem>
#include <fstream>
#include <intrin.h>
#include <algorithm>

// ---------- 平台特定工具 ----------
#ifdef _WIN32
#include <cstdlib>  // for _byteswap_ulong if needed
#endif
