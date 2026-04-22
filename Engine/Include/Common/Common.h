// ========== 合并后的 Common.h ==========
#pragma once

// ---------- 平台目标版本 ----------
#include <SDKDDKVer.h>

// ---------- Windows 优化 ----------
#define WIN32_LEAN_AND_MEAN // 从 Windows 头文件中排除极少使用的内容
#include <windows.h>

// ---------- 取消定义 Windows API 宏 ----------
#ifdef CreateWindow
#undef CreateWindow
#endif
// ... 其他 #undef ...

// ---------- C 运行时 ----------
#include <malloc.h>
#include <stdlib.h>

// ---------- C++ 标准库（高频） ----------
#include <chrono>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <sstream>
#include <string>
#include <vector>
