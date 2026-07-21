# 编辑器面板系统设计

> 定义编辑器 Panel 的统一接口、EditorLayout 的缩减后职责、及各面板的实现规范。

---

## 一、背景与目标

### 当前问题
- `EditorLayout` 持有具体面板的值成员（`m_assetBrowser`、`m_assetManager`），知晓过多细节
- 每新增一个面板需要在 `EditorLayout` 中新增 `Draw*()` 方法 + 显隐标志 + 状态转发
- 面板的显隐管理分散在 Layout 和面板之间，职责不清
- Console 等面板的复杂功能（过滤、类型筛选）在 Layout 中难以独立发展

### 目标
- **EditorLayout 缩减**：只做 dock 布局 + 面板注册遍历调用，不持有任何具体面板的成员
- **面板自治**：每个面板通过 `IEditorPanel` 接口表达其窗口名、draw、显隐、生命周期
- **消除转发层**：`Editor` 直接持有各面板，不再通过 Layout 中转
- **规范统一**：所有面板的 `Draw(float deltaTime)` 签名一致，遵守相同的绘制规范

---

## 二、IEditorPanel 接口

```cpp
// Editor/EditorLib/Core/IEditorPanel.h

#pragma once

#include <cstdint>
#include <string>

struct ImGuiID;

/// 编辑器面板的绘制阶段
enum class PanelDrawPhase : uint8_t {
    UI = 0, ///< 标准 ImGui UI 面板（AssetBrowser, Console, Properties 等）
};

/// @brief 编辑器面板基类
///
/// 所有在编辑器 Dock 空间中占有一块区域的面板必须继承此接口。
/// EditorLayout 只通过此接口与面板交互，不关心具体类型。
class IEditorPanel {
public:
    virtual ~IEditorPanel() = default;

    // ── 元信息（注册时使用，必须为常量） ──

    /// 窗口标识名，格式 "窗口标题###ID"
    /// 必须与 ImGui::Begin() 的第一个参数完全一致
    /// 例: "Console###Console"
    virtual const char* GetWindowName() const = 0;

    /// EditorStrings 的语言包 key
    /// 例: "console"
    virtual const char* GetWindowLabelKey() const = 0;

    /// 目标 Dock 节点 ID（由 EditorLayout 的 InitializeDockLayout 定义）
    virtual ImGuiID GetTargetDockId() const = 0;

    /// 绘制阶段（默认 UI）
    virtual PanelDrawPhase GetDrawPhase() const { return PanelDrawPhase::UI; }

    // ── 绘制 ──

    /// 每帧绘制（由 EditorLayout 在对应阶段统一调用）
    /// @param deltaTime 帧间隔时间
    virtual void Draw(float deltaTime) = 0;

    // ── 显隐管理 ──

    /// 面板当前是否可见
    virtual bool IsVisible() const = 0;

    /// 设置面板显隐（Editor 可通过视图菜单等外部途径控制）
    virtual void SetVisible(bool visible) = 0;

    // ── 生命周期（可选覆盖） ──

    /// 初始化（在 EditorLayout::RegisterPanel 之前或之后由 Editor 调用）
    virtual bool Initialize() { return true; }

    /// 清理
    virtual void Shutdown() {}
};
```

---

## 三、EditorLayout 缩减后的设计

```cpp
// Editor/EditorLib/Core/EditorLayout.h

#pragma once

#include "Boot/GameContext.h"
#include "IEditorPanel.h"
#include "ThirdParty/imgui/imgui.h"
#include <memory>
#include <vector>

class EditorLayout {
public:
    explicit EditorLayout(DX12Engine::Boot::GameContext *context);
    ~EditorLayout();

    EditorLayout(const EditorLayout &) = delete;
    EditorLayout &operator=(const EditorLayout &) = delete;

    bool Initialize();
    void Shutdown();

    /// 每帧绘制（由 Editor 在 ImGui::NewFrame 之后调用）
    void Draw(float deltaTime);

    /// 注册编辑器布局（通过 DebugUIManager 面板系统）
    void Register();

    /// 注册面板
    /// @param panel 面板指针（生命周期由调用方管理）
    void RegisterPanel(IEditorPanel *panel);

    /// 注销面板
    void UnregisterPanel(const char *windowName);

    // ── Dockspace ID（供面板 GetTargetDockId 引用） ──
    ImGuiID GetDockspaceId() const { return m_dockspaceId; }

    // ── 视口信息（仍由 Layout 管理，因为涉及 ImGui::Image 的 ImageButton 交互） ──
    void SetViewportSize(uint32_t w, uint32_t h);
    uint32_t GetViewportWidth() const { return m_viewportWidth; }
    uint32_t GetViewportHeight() const { return m_viewportHeight; }
    bool IsViewportHovered() const { return m_viewportHovered; }
    D3D12_GPU_DESCRIPTOR_HANDLE GetViewportSRV() const { return m_viewportSRV; }
    void SetViewportSRV(D3D12_GPU_DESCRIPTOR_HANDLE srv) { m_viewportSRV = srv; }

    /// 获取已注册的面板列表（供 Editor 遍历调用生命周期方法）
    const std::vector<IEditorPanel*> &GetPanels() const { return m_panels; }

private:
    void DrawMenuBar();
    void DrawDockSpace();
    void InitializeDockLayout(ImGuiID dockspaceId);

    DX12Engine::Boot::GameContext *m_context;

    // ── Dockspace ──
    ImGuiID m_dockspaceId = 0;
    bool m_dockLayoutInitialized = false;

    // ── 注册的面板 ──
    std::vector<IEditorPanel*> m_panels;

    // ── 视口 ──
    D3D12_GPU_DESCRIPTOR_HANDLE m_viewportSRV = {};
    uint32_t m_viewportWidth = 0;
    uint32_t m_viewportHeight = 0;
    bool m_viewportHovered = false;

    // ── 布局状态 ──
    bool m_initialized = false;
    bool m_showMenuBar = true;     // 菜单栏由 Layout 管理
    // 注：面板显隐由各面板自行管理，Layout 不再持有 m_showViewport 等标志
};
```

### Layout 的 Dock ID 定义（供各面板引用）

```cpp
// EditorLayout::InitializeDockLayout 中定义的 dock 拆分：

// 水平拆分为三列：左(20%) | 中(58%) | 右(22%)
ImGuiID dockMain = ImGui::DockBuilderSplitNode(dockspaceId, ImGuiDir_Left, 0.20f, &dockLeft, &dockMain);
ImGuiID dockRight = ImGui::DockBuilderSplitNode(dockMain, ImGuiDir_Right, 0.22f, nullptr, &dockMain);

// 左列垂直拆分：上(Outliner 60%) 下(Asset Manager 40%)
ImGuiID dockLeftBottom = ImGui::DockBuilderSplitNode(dockLeft, ImGuiDir_Down, 0.40f, nullptr, &dockLeft);

// 中间列垂直拆分：上(Viewport 70%) 下(Console 30%)
ImGuiID dockCenterBottom = ImGui::DockBuilderSplitNode(dockMain, ImGuiDir_Down, 0.30f, nullptr, &dockMain);

// 右侧（Properties，不拆分）
```

各面板的 `GetTargetDockId()` 返回对应的 ID 常量。EditorLayout 在 `InitializeDockLayout` 中遍历已注册面板，调用 `DockBuilderDockWindow(name, dockId)`。

---

## 四、ConsolePanel 示例

```cpp
// Editor/EditorLib/Panels/ConsolePanel.h

#pragma once

#include "Core/IEditorPanel.h"
#include "EditorConsoleSink.h"
#include <deque>
#include <memory>
#include <string>

/// @brief 控制台面板
///
/// 管理自己的 spdlog sink，支持日志级别过滤和关键字搜索。
/// 独立于 EditorLayout，自行决定显隐和筛选状态。
class ConsolePanel : public IEditorPanel {
public:
    ConsolePanel() = default;
    ~ConsolePanel() override = default;

    // ── IEditorPanel ──
    const char* GetWindowName() const override { return "Console###Console"; }
    const char* GetWindowLabelKey() const override { return "console"; }
    ImGuiID GetTargetDockId() const override;
    void Draw(float deltaTime) override;

    bool IsVisible() const override { return m_visible; }
    void SetVisible(bool visible) override { m_visible = visible; }

    bool Initialize() override;  // 创建 sink 并注册到 spdlog
    void Shutdown() override;    // 从 spdlog 移除 sink

    // ── 控制台特有接口 ──

    /// 获取日志条目（供 EditorLayout 等外部消费）
    std::vector<DX12Engine::Logger::ConsoleLogEntry> ConsumeEntries();

    /// 清空日志
    void Clear();

    /// 设置最大缓冲条目数
    void SetMaxEntries(size_t max);

private:
    void DrawFilterBar();        // 关键字搜索 + 级别过滤 + 类型过滤
    void DrawLogList();          // 虚拟滚动日志列表

    std::shared_ptr<DX12Engine::Logger::editor_console_sink_mt> m_sink;

    // 筛选状态
    char m_filterBuffer[128] = {};
    int m_minLevel = 0;        // 0=Trace .. 5=Critical
    bool m_showDebug = true;
    bool m_showInfo = true;
    bool m_showWarn = true;
    bool m_showError = true;

    // 显隐
    bool m_visible = true;

    // 缓冲
    std::deque<DX12Engine::Logger::ConsoleLogEntry> m_entries;
    size_t m_maxEntries = 2000;
};
```

```cpp
// ConsolePanel::Draw(float deltaTime) — 实现模板

void ConsolePanel::Draw(float deltaTime) {
    if (!m_visible) return;

    // 先消费最新日志
    auto newEntries = m_sink->ConsumeEntries();
    for (auto &e : newEntries) {
        // 筛选逻辑
        if (!m_showError && e.level >= spdlog::level::err) continue;
        // ...
        m_entries.push_back(std::move(e));
    }
    // 裁剪超限条目
    while (m_entries.size() > m_maxEntries)
        m_entries.pop_front();

    ImGui::Begin(GetWindowName(), &m_visible);

    DrawFilterBar();      // 搜索框 + 级别复选框
    ImGui::Separator();
    DrawLogList();        // 带颜色的日志虚拟滚动列表

    ImGui::End();
}
```

---

## 五、Editor 持有关系的变化

### 改造前（当前）
```
Editor
  ├── unique_ptr<EditorLayout>
  │     ├── EditorFileManager  m_assetBrowser    // Layout 持有
  │     ├── EditorAssetManager m_assetManager    // Layout 持有
  │     └── shared_ptr<console_sink>             // Layout 持有
  ├── PreviewManager          (值)               // Editor 持有
  ├── PreviewPBRRenderer      (值)               // Editor 持有
  ├── ThumbnailArray          (值)               // Editor 持有
  └── ...
```

### 改造后
```
Editor
  ├── unique_ptr<EditorLayout>                  // 只做布局分派
  ├── AssetBrowserPanel       m_assetBrowser    // 面板由 Editor 直接持有
  ├── ConsolePanel            m_consolePanel    // 面板由 Editor 直接持有
  ├── PreviewManager          (值)
  ├── PreviewPBRRenderer      (值)
  ├── ThumbnailArray          (值)
  └── ...

  Editor::Initialize() 中：
    m_layout->RegisterPanel(&m_assetBrowser);
    m_layout->RegisterPanel(&m_consolePanel);
    m_assetBrowser.SetPreviewContext(&m_previewManager, ...);  // 直接设置
```

---

## 六、Panel Draw 规范

所有 `IEditorPanel::Draw(float deltaTime)` 的实现必须遵守以下规范：

```
Draw(float deltaTime) 实现模板：
├── if (!m_visible) return;              // 早期返回
├── 消费外部数据（如有）                    // 如 Console 消费 sink 新条目
├── ImGui::Begin(GetWindowName(),        // 窗口名与 GetWindowName() 一致
├──             &m_visible, flags)
│   ├── 工具栏 / 筛选栏（如有）            // ImGui::InputText, Combo 等
│   ├── ImGui::Separator()               // 内容与控件的分隔
│   └── 主体内容                          // 虚拟滚动 / 图标网格 / 树形目录等
└── ImGui::End()
```

约束：
1. **窗口名一致**：`ImGui::Begin` 的第一个参数必须等于 `GetWindowName()` 的返回值，含 `###ID` 后缀
2. **显隐不依赖 Layout**：面板自行维护 `m_visible`，不得依赖 EditorLayout 管理显隐
3. **语言包**：窗口标题应使用 `EditorStrings::Get(GetWindowLabelKey(), "Fallback")`，保证多语言支持
4. **无渲染命令**：Panel 的 Draw 中禁止录制 GPU 命令（渲染命令在 FrameDriver 的渲染回调中进行）
5. **非阻塞**：Draw 中不得执行阻塞操作（文件 I/O、GPU 同步等），此类操作应通过 BackgroundExecutor 异步执行

---

## 七、面板注册与初始化时序

```
Editor::Initialize() 中：
(1) 创建所有面板对象（Editor 的值成员或 unique_ptr）
(2) 初始化面板（panel.Initialize()）
    - ConsolePanel::Initialize() → 创建 sink 并注册到 spdlog
    - ...
(3) EditorLayout::Initialize()
    - 初始化 ImGui Dockspace
    - 遍历已注册面板，调用 DockBuilderDockWindow
(4) 注册面板（m_layout->RegisterPanel(&panel)）
    - (3)(4) 的顺序：先 Initialize Layout（确保 dock 拆分已定义），再注册面板
(5) 注入面板间的依赖（如 m_assetBrowser.SetPreviewContext(...)）
(6) 开始主循环
```

---

## 八、新增面板的 checklist

当新增一个编辑器面板时：

1. **创建类继承 `IEditorPanel`**
   - 实现 `GetWindowName()` / `GetWindowLabelKey()` / `GetTargetDockId()`
   - 实现 `Draw(float)` / `IsVisible()` / `SetVisible()`
2. **在 `Editor::Initialize()` 中**
   - 创建面板对象
   - 调用 `panel.Initialize()`
   - `m_layout->RegisterPanel(&panel)`
   - 注入必要的依赖（context、预览管理器等）
3. **语言包**
   - 在 `editor_strings_zh-CN.json` / `editor_strings_en-US.json` 中添加对应 key
4. **Dock 布局**
   - 如需新的 dock 区域，在 `EditorLayout::InitializeDockLayout()` 中新增拆分子节点
   - 面板的 `GetTargetDockId()` 返回新节点的 ID