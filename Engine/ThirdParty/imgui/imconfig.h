// imconfig.h
#pragma once

// 禁用所有高级特性，避免版本冲突
#define IMGUI_DISABLE_OBSOLETE_FUNCTIONS
#define IMGUI_DISABLE_DEMO_WINDOWS
#define IMGUI_DISABLE_DEBUG_TOOLS

// 简化纹理类型
#define ImTextureID void *