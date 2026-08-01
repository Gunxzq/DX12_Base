#include "FileIconProvider.h"
#include "Asset/Definitions/AssetType.h"
#include <cctype>

using namespace DX12Engine::Resource;

// ========================================================================
// 扩展名 → AssetType
// ========================================================================
static AssetType ExtensionToAssetType(const std::string &ext) {
    // 统一转小写比较
    std::string lower;
    lower.reserve(ext.size());
    for (char c : ext)
        lower += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

    if (lower == ".dds" || lower == ".png" || lower == ".jpg" || lower == ".jpeg")
        return AssetType::Texture;
    if (lower == ".dxmesh" || lower == ".obj")
        return AssetType::Mesh;
    if (lower == ".material")
        return AssetType::Material;
    if (lower == ".bone")
        return AssetType::Skeleton;
    if (lower == ".anim")
        return AssetType::Animation;
    if (lower == ".character")
        return AssetType::Character;
    if (lower == ".scene")
        return AssetType::Scene;

    // 未知扩展名映射到 Scene 以外的类型，switch 中走 default
    return static_cast<AssetType>(255);
}

// ========================================================================
// 文件类型 → 颜色 + 图标
// ========================================================================
FileIconInfo GetFileIconInfo(const std::string &extension, bool isDirectory) {
    // 目录单独处理（不是资产类型）
    if (isDirectory) {
        return {IM_COL32(80, 70, 40, 220),
                "\xee\xb0\x97", // \uec17 文件夹图标
                "\xee\xb0\x97"};
    }

    switch (ExtensionToAssetType(extension)) {
    case AssetType::Texture:
        return {IM_COL32(60, 100, 80, 200),
                nullptr, // 预留缩略图
                "T"};

    case AssetType::Mesh:
        return {IM_COL32(80, 80, 120, 200),
                nullptr, // 预留缩略图
                "M"};

    case AssetType::Material:
        return {IM_COL32(100, 60, 80, 200), // 紫褐色
                "\xee\xae\x91",             // \ueb91 通用文件图标
                "\xee\xae\x91"};

    case AssetType::Scene:
        return {IM_COL32(120, 100, 40, 200), // 金色
                "\xee\xae\x91",              // \ueb91 通用文件图标
                "\xee\xae\x91"};

    case AssetType::Skeleton:
        return {IM_COL32(100, 90, 130, 200), // 青紫色（骨骼）
                "\xee\xae\x91",              // \ueb91 通用文件图标
                "S"};

    case AssetType::Animation:
        return {IM_COL32(140, 90, 50, 200), // 橙褐色（动画）
                "\xee\xae\x91",             // \ueb91 通用文件图标
                "A"};

    case AssetType::Character:
        return {IM_COL32(70, 110, 130, 200), // 蓝青色（角色）
                "\xee\xae\x91",              // \ueb91 通用文件图标
                "C"};

    case AssetType::Terrain:
        return {IM_COL32(60, 120, 60, 200), // 草绿色
                "\xee\xae\x91",             // \ueb91 通用文件图标
                "\xee\xae\x91"};

    default:
        return {IM_COL32(80, 80, 100, 200),
                "\xee\xae\x91", // \ueb91 通用文件图标
                "\xee\xae\x91"};
    }
}