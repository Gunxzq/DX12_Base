#pragma once

#include "ThirdParty/imgui/imgui.h"
#include <string>

struct FileIconInfo {
    ImU32 color = 0;
    const char *iconChar = nullptr; // iconfont Unicode 字符 (UTF-8 encoded)，nullptr 表示无图标
    const char *label = "?";        // 回退文本标签
};

/**
 * @brief 根据文件扩展名和是否为目录获取图标信息
 * @param extension 文件扩展名（如 ".dxmesh"）
 * @param isDirectory 是否为目录
 * @return FileIconInfo 包含颜色、图标字符、回退标签
 */
FileIconInfo GetFileIconInfo(const std::string &extension, bool isDirectory);