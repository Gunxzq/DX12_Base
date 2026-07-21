#include "ConsolePanel.h"
#include "EditorStrings.h"
#include "ThirdParty/imgui/imgui.h"
#include "ThirdParty/imgui/imgui_internal.h"
#include <algorithm>
#include <spdlog/spdlog.h>

bool ConsolePanel::Initialize() {
    if (m_sink)
        return true;

    m_sink = std::make_shared<DX12Engine::Logger::editor_console_sink_mt>();
    m_sink->set_level(spdlog::level::trace);
    if (auto logger = spdlog::default_logger()) {
        logger->sinks().push_back(m_sink);
    }
    return true;
}

void ConsolePanel::Shutdown() {
    if (m_sink) {
        if (auto logger = spdlog::default_logger()) {
            auto &sinks = logger->sinks();
            auto it = std::find(sinks.begin(), sinks.end(), m_sink);
            if (it != sinks.end()) {
                sinks.erase(it);
            }
        }
        m_sink.reset();
    }
    m_entries.clear();
}

std::vector<DX12Engine::Logger::ConsoleLogEntry> ConsolePanel::ConsumeEntries() {
    if (m_sink) {
        return m_sink->ConsumeEntries();
    }
    return {};
}

void ConsolePanel::Clear() {
    if (m_sink) {
        m_sink->ConsumeEntries();
    }
    m_entries.clear();
}

void ConsolePanel::Draw(float deltaTime) {
    if (!m_visible)
        return;

    // ── 消费新日志 ──
    {
        bool atBottom = false;
        if (ImGui::GetCurrentWindow()) {
            atBottom = ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 5.0f;
        }

        if (m_sink) {
            auto entries = m_sink->ConsumeEntries();
            if (!entries.empty()) {
                for (auto &entry : entries) {
                    m_entries.emplace_back(std::move(entry));
                }
                while (m_entries.size() > m_maxEntries) {
                    m_entries.pop_front();
                }
                // 仅在用户已滚动到底部时自动跟随
                if (atBottom) {
                    ImGui::SetScrollHereY(1.0f);
                }
            }
        }
    }

    ImGui::Begin(GetWindowName(), &m_visible);

    // ── 工具栏 ──
    DrawFilterBar();

    ImGui::Separator();

    // ── 日志列表 ──
    DrawLogList();

    ImGui::End();
}

void ConsolePanel::DrawFilterBar() {
    if (ImGui::SmallButton(EditorStrings::Get("console_clear", "Clear"))) {
        Clear();
    }
    ImGui::SameLine();
    ImGui::TextDisabled("%zu lines", m_entries.size());
}

void ConsolePanel::DrawLogList() {
    ImGui::BeginChild("LogScroll", ImVec2(0, 0), false, ImGuiWindowFlags_HorizontalScrollbar);

    ImGuiListClipper clipper;
    clipper.Begin(static_cast<int>(m_entries.size()));
    while (clipper.Step()) {
        for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; i++) {
            const auto &entry = m_entries[i];
            ImVec4 color;
            switch (entry.level) {
            case 0: // Trace
                color = ImVec4(0.5f, 0.5f, 0.5f, 1.0f);
                break;
            case 1: // Debug
                color = ImVec4(0.7f, 0.7f, 0.7f, 1.0f);
                break;
            case 2: // Info
                color = ImVec4(0.3f, 0.8f, 0.3f, 1.0f);
                break;
            case 3: // Warn
                color = ImVec4(0.9f, 0.8f, 0.1f, 1.0f);
                break;
            case 4: // Error
                color = ImVec4(0.9f, 0.2f, 0.2f, 1.0f);
                break;
            case 5: // Critical
                color = ImVec4(1.0f, 0.1f, 0.1f, 1.0f);
                break;
            default:
                color = ImVec4(1.0f, 1.0f, 1.0f, 1.0f);
                break;
            }
            // 移除末尾换行符，避免 ImGui 空行
            const auto &text = entry.formatted;
            if (!text.empty() && (text.back() == '\n' || text.back() == '\r')) {
                ImGui::TextColored(color, "%s", text.substr(0, text.size() - 1).c_str());
            } else {
                ImGui::TextColored(color, "%s", text.c_str());
            }
        }
    }

    ImGui::EndChild();
}