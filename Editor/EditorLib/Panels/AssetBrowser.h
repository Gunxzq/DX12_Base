#pragma once

#include "Core/IEditorPanel.h"
#include "Preview/PreviewContext.h"
#include "ThirdParty/imgui/imgui.h"
#include <d3d12.h>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>
#include <windows.h>
#include <wrl/client.h>

namespace DX12Engine::Renderer {
class CommandManager;
class D3D12DeviceContext;
class FrameScratchAllocator;
} // namespace DX12Engine::Renderer
class PreviewManager;
class ThumbnailArray;
class PreviewPBRRenderer;
namespace DX12Engine::Boot {
class GameContext;
}
namespace DX12Engine::Resource {
struct SceneDescription;
} // namespace DX12Engine::Resource
#include "Scene/SceneConstructor.h"

// ========================================================================
// EditorAssetManager — 资产管理器面板
//
// 功能：
//   - 面包屑路径导航（可点击切换目录）
//   - 左侧：目录树
//   - 右侧：大图标视图（使用 Windows 系统图标）
//   - 双击 .dxmesh 等资产文件时触发回调
// ========================================================================

class EditorAssetManager : public IEditorPanel {
public:
    /// 资产文件双击回调：void(const std::string& filePath)
    using AssetDoubleClickCallback = std::function<void(const std::string &)>;

    EditorAssetManager();
    ~EditorAssetManager();

    EditorAssetManager(const EditorAssetManager &) = delete;
    EditorAssetManager &operator=(const EditorAssetManager &) = delete;

    void SetContentRoot(const std::string &root);
    void SetDevice(ID3D12Device *device) { m_device = device; }
    void SetCommandManager(DX12Engine::Renderer::CommandManager *cmdMgr) { m_cmdMgr = cmdMgr; }

    // ── IEditorPanel ──
    const char *GetWindowName() const override { return "Asset Manager###AssetManager"; }
    const char *GetWindowLabelKey() const override { return "asset_manager"; }
    DockWindowId GetDockWindowId() const override { return DockWindowId::AssetManager; }
    DockZone GetDockZone() const override { return DockZone::CenterBottom; }
    void Draw(float deltaTime) override; // deltaTime 当前未使用，保留接口签名一致性

    bool IsVisible() const { return m_visible; }
    void SetVisible(bool visible) { m_visible = visible; }

    /// 设置资产文件双击回调
    void SetOnFileDoubleClick(AssetDoubleClickCallback callback) { m_onFileDoubleClick = std::move(callback); }

    /// 设置当前预览中的资产路径（用于图标高亮标识）
    void SetPreviewHighlightPath(const std::string &path) { m_previewHighlightPath = path; }

    /// 设置缩略图数组引用（供 DrawContentIcons 显示缩略图）
    void SetThumbnailArray(ThumbnailArray *array) { m_thumbnailArray = array; }

    /// 设置预览渲染上下文（由 Editor 提供，注册预览渲染回调及双击回调）
    void SetPreviewContext(PreviewManager *previewMgr, PreviewPBRRenderer *previewRenderer,
                           ThumbnailArray *thumbnailArray, DX12Engine::Renderer::FrameScratchAllocator *scratchAlloc,
                           DX12Engine::Boot::GameContext *gameContext);
    void SetLayoutProxy(std::function<void(PreviewId)> onSetPreviewId, std::function<void()> onShowPreviewPanel);

    /// 设置场景切换回调（加载新场景前调用，用于释放旧场景资源）
    /// @return true 表示需要继续加载场景，false 表示场景已存在，无需重复加载
    void SetSceneSwitcher(std::function<bool(const std::string &, const std::filesystem::path &)> onSwitchScene);

    /// 异步加载场景描述（供 Editor 启动默认场景等使用）
    /// SceneConstructor 生命周期由内部管理，加载完成后自动释放
    void LoadSceneDescription(const DX12Engine::Resource::SceneDescription &desc);

    /// 从文件路径异步加载场景（不触发 SwitchScene 回调，供 Tab 切换使用）
    /// @param sceneFilePath .scene.json 文件路径
    void LoadSceneFromFile(const std::filesystem::path &sceneFilePath);

    /// 注册一个缩略图：将文件路径映射到 GPU 句柄
    void RegisterThumbnail(const std::string &filePath, D3D12_GPU_DESCRIPTOR_HANDLE gpuHandle, uint32_t slice);

    /// 缩略图映射条目
    struct ThumbnailEntry {
        D3D12_GPU_DESCRIPTOR_HANDLE gpuHandle = {};
        uint32_t slice = UINT32_MAX;
    };

    /// 移除所有指向指定 slice 的缩略图映射（当 slice 被复用时应调用）
    void RemoveThumbnailsBySlice(uint32_t slice);

    /// 获取缩略图映射表（供 Editor 在关闭时遍历写入磁盘缓存）
    const std::unordered_map<std::string, ThumbnailEntry> &GetThumbnailMap() const { return m_thumbnailMap; }

    /// 启动时从磁盘加载缓存的缩略图到 ThumbnailArray
    void LoadThumbnailPack(DX12Engine::Renderer::D3D12DeviceContext *deviceCtx);

    /// 预览状态查询（供 Editor Run 循环使用）
    PreviewId GetDetailPreviewId() const { return m_detailPreviewId; }
    bool NeedsThumbnailCache() const { return m_needsThumbnailCache; }
    void ClearThumbnailCacheFlag() { m_needsThumbnailCache = false; }
    const std::string &GetPreviewFilePath() const { return m_previewFilePath; }

private:
    void ScanDirectory(const std::filesystem::path &dirPath);
    void DrawBreadcrumb();
    void DrawDirectoryTree(const std::filesystem::path &dirPath);
    void DrawContentIcons();
    void RegisterPreviewRenderCallback();
    void OnFileDoubleClick(const std::string &filePath);

    /// 场景切换回调（由 Editor 注册，返回 true 表示需要继续加载场景）
    std::function<bool(const std::string &, const std::filesystem::path &)> m_onSwitchScene;

    /// 获取文件/文件夹的 Windows 系统图标（返回 ImTextureID 用于 ImGui::Image）
    ImTextureID GetIconTexture(const std::string &extension, bool isDirectory);

    std::string m_contentRoot;
    bool m_visible = true;
    ID3D12Device *m_device = nullptr;
    DX12Engine::Renderer::CommandManager *m_cmdMgr = nullptr;

    std::filesystem::path m_currentPath;

    struct DirEntry {
        std::filesystem::path path;
        std::string name;
        bool isDirectory = false;
        std::string extension;
    };
    std::vector<DirEntry> m_entries;

    // 图标缓存
    struct IconCacheEntry {
        Microsoft::WRL::ComPtr<ID3D12Resource> texture;
        D3D12_GPU_DESCRIPTOR_HANDLE gpuHandle = {};
        D3D12_CPU_DESCRIPTOR_HANDLE cpuHandle = {};
    };
    std::unordered_map<std::string, IconCacheEntry> m_iconCache;

    /// 资产文件双击回调
    AssetDoubleClickCallback m_onFileDoubleClick;

    float m_iconSize = 72.0f;
    float m_iconSpacing = 10.0f;

    /// 当前预览中的资产路径（用于图标高亮标识）
    std::string m_previewHighlightPath;

    /// 缩略图数组（来自 Editor，用于显示缩略图）
    ThumbnailArray *m_thumbnailArray = nullptr;

    /// 文件路径 → 缩略图 GPU SRV 句柄
    std::unordered_map<std::string, ThumbnailEntry> m_thumbnailMap;

    // ── 预览渲染上下文（由 Editor 注入） ──
    PreviewManager *m_previewMgr = nullptr;
    PreviewPBRRenderer *m_previewRenderer = nullptr;
    ThumbnailArray *m_thumbnailArrayForRender = nullptr;
    DX12Engine::Renderer::FrameScratchAllocator *m_scratchAlloc = nullptr;
    DX12Engine::Boot::GameContext *m_gameCtx = nullptr;

    // ── 预览状态（由 AssetBrowser 自管理） ──
    PreviewId m_detailPreviewId = 0;
    uint64_t m_previewLoadSequence = 0;
    std::string m_previewFilePath;
    bool m_needsThumbnailCache = false;

    // ── 布局代理回调（AssetBrowser 通知 Layout 更新 UI） ──
    std::function<void(PreviewId)> m_onSetPreviewId;
    std::function<void()> m_onShowPreviewPanel;

    // ── 当前正在加载的 SceneConstructor（编辑器生命周期，值成员，复用避免悬空回调） ──
    DX12Engine::Scene::SceneConstructor m_sceneCtor;
};