#pragma once

#include "Boot/GameContext.h"
#include "ECS/Core/Registry.h"
#include "Panels/AssetBrowser.h"
#include "Panels/ConsolePanel.h"
#include "Panels/FileBrowser.h"
#include "Panels/OutlinerPanel.h"
#include "Preview/PreviewCacheManager.h"
#include "Preview/PreviewManager.h"
#include "Preview/PreviewPBRRenderer.h"
#include "Preview/ThumbnailArray.h"
#include "Renderer/FrameResources/FrameScratchAllocator.h"
#include "Renderer/Pipeline/LightingRenderer.h"
#include "Renderer/Pipeline/OpaqueRenderer.h"
#include "Renderer/RenderItemBuilder/OpaqueRenderItemBuilder.h"
#include "Renderer/RenderItemBuilder/TRenderQueue.h"
#include "Scene/EditorSceneManager.h"
#include <memory>
#include <wrl/client.h>

// 前向声明
class EditorLayout;
class EditorViewport;
//class EditorViewportInput; // [已注释] 旧输入系统，已替换为 EditorCameraSystem
class EditorCameraSystem;
class EditorViewportToolbar;

namespace DX12Engine::Renderer {
class D3D12DeviceContext;
class FrameScratchAllocator;
class PreviewManager;
} // namespace DX12Engine::Renderer

// ========================================================================
// Editor - 编辑器主逻辑层，负责运行主循环和组合编辑器模块
//
// 面板持有关系：
//   - Editor 直接持有所有面板（值成员）
//   - 在 Initialize() 中通过 m_layout->RegisterPanel() 注册
//   - 不再通过 EditorLayout 转发依赖
// ========================================================================

class Editor {
public:
    explicit Editor(DX12Engine::Boot::GameContext *context);
    ~Editor();

    Editor(const Editor &) = delete;
    Editor &operator=(const Editor &) = delete;
    Editor(Editor &&) = delete;
    Editor &operator=(Editor &&) = delete;

    bool Initialize();
    int Run();
    void Shutdown();

    bool IsRunning() const { return m_isRunning; }

private:
    void RegisterEngineSystems();             // WindowResizeSystem, FullscreenSystem 等引擎级系统
    void RegisterEditorRenderSystems();       // 构建器 + 实体渲染器 System
    void FinalizePreviewMesh();               // 轮询等待 GPU 就绪
    void CachePreviewThumbnail(PreviewId id); // 缓存预览结果到缩略图

private:
    DX12Engine::Boot::GameContext *m_context;
    std::unique_ptr<EditorLayout> m_layout;
    std::unique_ptr<EditorViewport> m_viewport;
    //std::unique_ptr<EditorViewportInput> m_viewportInput; // [已注释] 旧输入系统，已替换为 EditorCameraSystem
    std::unique_ptr<EditorCameraSystem> m_cameraSystem;
    std::unique_ptr<EditorViewportToolbar> m_toolbar;

    // ── 面板（Editor 直接持有，注册到 EditorLayout） ──
    EditorAssetManager m_assetManager; // 资产管理器
    EditorFileManager m_assetBrowser;  // 文件浏览器
    ConsolePanel m_consolePanel;       // 控制台
    OutlinerPanel m_outlinerPanel;     // 场景大纲

    // 资产预览系统
    PreviewManager m_previewManager;
    PreviewCacheManager m_previewCache;
    DX12Engine::Renderer::FrameScratchAllocator m_scratchAllocator;
    ThumbnailArray m_thumbnailArray;

    // 预览 PBR 渲染器
    PreviewPBRRenderer m_previewRenderer;

    // ── 场景管理器（组合包装 Bootstrap 的 SceneManager） ──
    EditorSceneManager m_editorSceneMgr;

    // ── 场景渲染管线 ──
    std::unique_ptr<DX12Engine::Renderer::OpaqueRenderer> m_opaqueRenderer;
    std::unique_ptr<DX12Engine::Renderer::LightingRenderer> m_lightingRenderer;
    std::unique_ptr<DX12Engine::Renderer::OpaqueRenderItemBuilder> m_opaqueBuilder;
    DX12Engine::Renderer::TRenderQueue<DX12Engine::Renderer::OpaqueRenderItem> m_opaqueQueue;

    bool m_isRunning = false;
    bool m_isInitialized = false;
};