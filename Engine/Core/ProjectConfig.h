#pragma once
#include <string>

namespace DX12Engine::Core {

// ============================================================================
// ProjectConfig — 项目配置（非生成，普通头文件）
//
// 由 Game/Editor 的 main.cpp 从 CMake 生成的 ProjectConfigGenerated.h 填充，
// 然后传入 Bootstrap::Run() 做缓存。Engine 库代码只包含本文件，不包含生成头。
// ============================================================================
struct ProjectConfig {
    std::string Name;
    std::string Type;
    std::string Root;         // 项目根目录（源码根）
    std::string ConfigRoot;   // 配置文件目录（如 "Game/Config"）
    std::string ContentRoot;  // 资源目录（如 ".../Content"）
    std::string ShaderRoot;   // 着色器目录（如 ".../Shaders"）
};

} // namespace DX12Engine::Core
