#pragma once

#include "Core/IEditorPanel.h"
#include "EditorConsoleSink.h"
#include <deque>
#include <memory>
#include <string>

// ========================================================================
// ConsolePanel — 编辑器控制台面板
//
// 职责：
//   - 管理自己的 spdlog sink，独立于 EditorLayout
//   - 支持日志级别过滤和关键字搜索
//   - 支持自动滚动跟随
// ========================================================================

class ConsolePanel : public IEditorPanel {
public:
    ConsolePanel() = default;
    ~ConsolePanel() override = default;

    ConsolePanel(const ConsolePanel &) = delete;
    ConsolePanel &operator=(const ConsolePanel &) = delete;

    // ── IEditorPanel ──
    const char *GetWindowName() const override { return "Console###Console"; }
    const char *GetWindowLabelKey() const override { return "console"; }
    DockWindowId GetDockWindowId() const override { return DockWindowId::Console; }
    DockZone GetDockZone() const override { return DockZone::CenterBottom; }
    void Draw(float deltaTime) override;

    bool IsVisible() const override { return m_visible; }
    void SetVisible(bool visible) override { m_visible = visible; }

    bool Initialize() override;  // 创建 sink 并注册到 spdlog
    void Shutdown() override;    // 从 spdlog 移除 sink

    // ── 控制台特有接口 ──

    /// 获取日志条目（供外部消费）
    std::vector<DX12Engine::Logger::ConsoleLogEntry> ConsumeEntries();

    /// 清空日志
    void Clear();

    /// 设置最大缓冲条目数
    void SetMaxEntries(size_t max) { m_maxEntries = max; }

    /// 获取当前日志条目数
    size_t GetEntryCount() const { return m_entries.size(); }

private:
    void DrawFilterBar();
    void DrawLogList();

    std::shared_ptr<DX12Engine::Logger::editor_console_sink_mt> m_sink;

    // 筛选状态
    char m_filterBuffer[128] = {};
    int m_minLevel = 0; // 0=Trace .. 5=Critical

    // 显隐
    bool m_visible = true;

    // 缓冲
    std::deque<DX12Engine::Logger::ConsoleLogEntry> m_entries;
    size_t m_maxEntries = 5000;
};