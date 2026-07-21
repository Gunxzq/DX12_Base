#pragma once

#include <cstdint>

/// @brief 编辑器面板的绘制阶段
enum class PanelDrawPhase : uint8_t {
    UI = 0, ///< 标准 ImGui UI 面板（AssetBrowser, Console, Properties 等）
};

/// @brief 编辑器 Dock 区域（用于面板注册时指定目标位置）
///
/// EditorLayout 在 InitializeDockLayout 中将这些枚举值映射到实际的 ImGuiID。
enum class DockZone : uint8_t {
    Left,         ///< 左列上（Outliner）
    LeftBottom,   ///< 左列下（Asset Manager / Content Browser）
    Center,       ///< 中间上（Viewport）
    CenterBottom, ///< 中间下（Console）
    Right,        ///< 右列（Properties）
};

/// @brief 窗口内部标识符（###ID 后缀）
///
/// 所有面板的 GetWindowName() 必须返回 "显示名###DockWindowId::Xxx" 格式。
/// 此枚举确保 ###ID 部分在 DockBuilderDockWindow 和 ImGui::Begin 之间完全一致。
enum class DockWindowId : uint32_t {
    Viewport,
    Outliner,
    Properties,
    Console,
    AssetManager,
    ContentBrowser,
};

/// 将 DockWindowId 转换为 "###xxx" 字符串
inline const char *DockWindowIdToStr(DockWindowId id) {
    switch (id) {
    case DockWindowId::Viewport:
        return "###Viewport";
    case DockWindowId::Outliner:
        return "###Outliner";
    case DockWindowId::Properties:
        return "###Properties";
    case DockWindowId::Console:
        return "###Console";
    case DockWindowId::AssetManager:
        return "###AssetManager";
    case DockWindowId::ContentBrowser:
        return "###ContentBrowser";
    default:
        return "###Unknown";
    }
}

/// @brief 编辑器面板基类
///
/// 所有在编辑器 Dock 空间中占有一块区域的面板必须继承此接口。
/// EditorLayout 只通过此接口与面板交互，不关心具体类型。
class IEditorPanel {
public:
    virtual ~IEditorPanel() = default;

    // ── 元信息（注册时使用，必须为常量） ──

    /// 窗口标识名，格式 "显示名###xxx"
    /// 必须与 ImGui::Begin() 的第一个参数完全一致
    virtual const char *GetWindowName() const = 0;

    /// EditorStrings 的语言包 key
    virtual const char *GetWindowLabelKey() const = 0;

    /// 窗口内部标识符（用于 dock 绑定，确保 ###ID 一致）
    virtual DockWindowId GetDockWindowId() const = 0;

    /// 目标 Dock 区域
    virtual DockZone GetDockZone() const = 0;

    /// 绘制阶段（默认 UI）
    virtual PanelDrawPhase GetDrawPhase() const { return PanelDrawPhase::UI; }

    // ── 绘制 ──

    /// 每帧绘制（由 EditorLayout 在对应阶段统一调用）
    virtual void Draw(float deltaTime) = 0;

    // ── 显隐管理 ──

    /// 面板当前是否可见
    virtual bool IsVisible() const = 0;

    /// 设置面板显隐
    virtual void SetVisible(bool visible) = 0;

    // ── 生命周期（可选覆盖） ──

    virtual bool Initialize() { return true; }
    virtual void Shutdown() {}
};