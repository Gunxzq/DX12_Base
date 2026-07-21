# 编辑器目录结构与预览系统重构快照 (2026-07-17)

## 当前实现状态

### ✅ 已完成

| 模块 | 状态 |
|:-----|:------|
| 目录结构重组 — `Core/`、`Panels/`、`Preview/`、`Scene/`、`Input/`、`Log/` | ✅ |
| 命名空间清理 — Preview 类移除 `DX12Engine::Renderer` 命名空间 | ✅ |
| 预览 PSO 双模式 — PBR（材质/网格）+ Unlit（纹理） | ✅ |
| 预览渲染回调迁移至 AssetBrowser | ✅ |
| 双击回调迁移至 AssetBrowser（`OnFileDoubleClick`） | ✅ |
| 状态管理迁移 — `m_detailPreviewId`、`m_previewFilePath` 等移至 AssetBrowser | ✅ |
| EditorLayout 移除冗余转发方法 | ✅ |
| `.mat` 文件双击预览支持 | ✅ |
| 预览面板合并到属性面板 | ✅ |
| 光照参数控件（强度/角度滑块） | ✅ |
| 方向光强度默认值 1.0 → 3.0 | ✅ |
| **OutlinerPanel 独立为 IEditorPanel** — 持有 EditorScene，管理场景 ECS 数据 | ✅ |
| **NameComponent** — `persistentId`（单调递增 uint64_t）+ `name`（字符串） | ✅ |
| **FocusSelection（F 键）** — 聚焦选中实体，保持视角方向仅调位置 | ✅ |
| **Outliner 专用输入上下文** — priority 25，支持 FocusSelection/Delete/Duplicate | ✅ |
| **视口输入修复** — 用 `m_viewportHovered` 替代 `ImGui::WantCaptureMouse` | ✅ |
| **InputContextStack 日志回调** — `SetLogCallback(std::function)` 引擎 Core 层接口 | ✅ |

### 📁 新目录结构

```
Editor/EditorLib/
  Core/                  ← 编辑器核心框架
    Editor.h/.cpp
    EditorLayout.h/.cpp
    EditorStrings.h/.cpp
    IEditorPanel.h
  Panels/                ← UI 面板
    AssetBrowser.h/.cpp      ← 原名 EditorAssetManager
    ConsolePanel.h/.cpp      ← 控制台面板
    FileBrowser.h/.cpp       ← 原名 EditorFileManager
    FileIconProvider.h/.cpp
    OutlinerPanel.h/.cpp     ← 场景大纲（持有 EditorScene）
  Preview/               ← 预览系统
    PreviewContext.h
    PreviewManager.h/.cpp
    PreviewPBRRenderer.h/.cpp
    PreviewCacheManager.h/.cpp
    ThumbnailArray.h/.cpp
  Scene/                 ← 场景 + 视口
    EditorScene.h/.cpp
    EditorViewport.h/.cpp
  Input/                 ← 输入处理
    EditorViewportInput.h/.cpp
    EditorViewportInputActions.h
  Log/                   ← 控制台日志
    EditorConsoleSink.h/.cpp
```

### 🔄 当前架构

```
Editor::Initialize()
  ├── 初始化预览系统（PreviewManager, ThumbnailArray, PreviewPBRRenderer, etc.）
  ├── 初始化布局（EditorLayout）
  ├── m_outlinerPanel.InitializeContext(context) + LoadDefaultScene()
  │     └── OutlinerPanel 持有 EditorScene，管理场景 ECS 数据
  ├── SetPreviewContext() → AssetBrowser
  ├── SetLayoutProxy() → 通知 Layout 更新 UI
  └── RegisterPanel(&m_assetManager, &m_assetBrowser, &m_consolePanel, &m_outlinerPanel)

EditorLayout 职责（已精简）
  ├── DrawDockSpace() — 仅定义布局框架
  ├── DrawViewport() — 视口展示（SRV → ImGui::Image）
  ├── DrawProperties() — 属性面板 + 预览
  ├── DrawStatusBar()
  └── 遍历已注册的 IEditorPanel 调用 Draw()

输入上下文栈（优先级从高到低）
  ├── Outliner (priority 25) — FocusSelection, Delete, Duplicate, Select
  ├── UI (priority 20) — Select, Delete, Undo, Redo, Save, Open, SaveAs
  ├── Viewport (priority 15) — Move, Look, Zoom, Pan, FocusSelection, OrbitCamera
  └── Gameplay (priority 10) — 常驻默认上下文
```

### ⚙️ 关键集成点

| 集成点 | 位置 |
|:-------|:------|
| 预览渲染上下文注入 | `Editor::Initialize()` → `m_layout->SetPreviewContext(...)` |
| 布局代理回调 | `Editor::Initialize()` → `AssetBrowser::SetLayoutProxy(...)` |
| 场景管理 | `OutlinerPanel` 持有 `EditorScene`，外部通过 `GetEditorScene()` 访问 |
| 实体选中 | `OutlinerPanel::GetSelectedEntity()` |
| F 键聚焦 | ImmediateCallback 中检测 `ActionId_FocusSelection` → `m_viewportInput->FocusOnEntity()` |
| Outliner 上下文切换 | ImmediateCallback 中通过 `OutlinerPanel::IsOutlinerFocused()` 驱动 |
| 视口输入控制 | 基于 `m_viewportHovered` 判断，不再依赖 `ImGui::WantCaptureMouse` |
| 引擎 Core 层日志 | `InputContextStack::SetLogCallback()` 回调接口 |

### 🔧 已知问题

- 缩略图磁盘缓存异步路径未实现（`CachePreviewThumbnail` 中 TODO）
- `ReadbackSlice` 未与 DIRECT 渲染队列做 fence 同步
- `FinalizePreviewMesh` 仍在 Editor 中，通过 `m_layout->GetAssetManager()` 访问 AssetBrowser 状态
- `CachePreviewThumbnail` 仍在 Editor 中，同样通过 `GetAssetManager()` 访问状态
- 编辑器视口相机运动与 Game 端存在差异（右键旋转时渲染表现异常，待排查）