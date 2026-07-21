#pragma once

#include "Core/IEditorPanel.h"
#include <filesystem>
#include <string>
#include <vector>

// ========================================================================
// EditorFileManager — 资源浏览器面板
//
// 两种视图模式（类似 VS2022）：
//   1. 文件夹视图 — 展示 Content 目录的真实文件树
//   2. 文件类型视图 — 按文件类型分类显示
//
// 支持搜索过滤
// 路径统一使用 PathUtils::Normalize() 处理（'/' 分隔符）
// ========================================================================

class EditorFileManager : public IEditorPanel {
public:
    EditorFileManager();
    ~EditorFileManager() = default;

    EditorFileManager(const EditorFileManager &) = delete;
    EditorFileManager &operator=(const EditorFileManager &) = delete;

    /// 设置 Content 根目录
    void SetContentRoot(const std::string &root);

    // ── IEditorPanel ──
    const char *GetWindowName() const override { return "Content Browser###ContentBrowser"; }
    const char *GetWindowLabelKey() const override { return "content_browser"; }
    DockWindowId GetDockWindowId() const override { return DockWindowId::ContentBrowser; }
    DockZone GetDockZone() const override { return DockZone::LeftBottom; }
    void Draw(float deltaTime) override; // deltaTime 当前未使用，保留接口签名一致性

    /// 面板可见性
    bool IsVisible() const { return m_visible; }
    void SetVisible(bool visible) { m_visible = visible; }

private:
    /// 扫描 Content 目录下的所有文件
    void ScanContentDirectory();

    /// 递归绘制目录树节点（文件夹视图）
    void DrawDirectoryNode(const std::filesystem::path &dirPath, const std::string &searchStr);

    /// 按类型绘制文件列表（文件类型视图）
    void DrawTypeView(const std::string &searchStr);

    /// 根据文件扩展名判断类型
    static const char *GetFileTypeCategory(const std::string &ext);

    /// 检查文件名是否匹配搜索关键字（大小写不敏感）
    static bool MatchesSearch(const std::string &filename, const std::string &search);

    std::string m_contentRoot;
    bool m_visible = true;

    // 视图模式
    enum class ViewMode { Folder, Type };
    ViewMode m_viewMode = ViewMode::Folder;

    // 搜索过滤
    char m_searchBuffer[128] = {};

    // 文件树缓存（根目录下的直接子目录/文件列表）
    struct FileEntry {
        std::filesystem::path path;
        std::string filename;
        std::string normalizedPath;
        std::string extension;
        bool isDirectory = false;
    };
    std::vector<FileEntry> m_rootEntries;
    std::vector<FileEntry> m_allFiles; // 所有文件平铺（用于类型视图）
};