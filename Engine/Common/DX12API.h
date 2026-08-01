#pragma once

// ========================================================================
// DX12API — DLL 导出/导入宏
//
// 每个 DLL 在 CMake 中定义对应的 BUILD_xxx 宏，导出符号时使用。
// 引用方（exe 或其他 DLL）没有该宏，自动使用 dllimport。
//
// 用法：
//   class DX12ECS_API MyClass { ... };
//   void DX12ECS_API MyFunction();
//   extern DX12ECS_API int MyGlobal;
// ========================================================================

// ── 各模块的 BUILD 宏在 CMakeLists.txt 中用 target_compile_definitions 定义 ──

#pragma once

#ifdef DX12ECS_BUILD
#  define DX12ECS_API __declspec(dllexport)
#else
#  define DX12ECS_API __declspec(dllimport)
#endif

#ifdef DX12CORE_BUILD
#  define DX12CORE_API __declspec(dllexport)
#else
#  define DX12CORE_API __declspec(dllimport)
#endif

#ifdef DX12RESOURCE_BUILD
#  define DX12RESOURCE_API __declspec(dllexport)
#else
#  define DX12RESOURCE_API __declspec(dllimport)
#endif

#ifdef DX12RENDERER_BUILD
#  define DX12RENDERER_API __declspec(dllexport)
#else
#  define DX12RENDERER_API __declspec(dllimport)
#endif

#ifdef DX12EDITOR_BUILD
#  define DX12EDITOR_API __declspec(dllexport)
#else
#  define DX12EDITOR_API __declspec(dllimport)
#endif

#ifdef DX12NETWORK_BUILD
#  define DX12NETWORK_API __declspec(dllexport)
#else
#  define DX12NETWORK_API __declspec(dllimport)
#endif

#ifdef DX12BACKGROUND_BUILD
#  define DX12BACKGROUND_API __declspec(dllexport)
#else
#  define DX12BACKGROUND_API __declspec(dllimport)
#endif
