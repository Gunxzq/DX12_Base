#include "Core/DebugOverlay/DebugOverlay.h"
#include <algorithm>

namespace DX12Engine {
namespace Core {

LogEntry::LogEntry(Level lvl, std::string msg, std::string fmt)
    : timestamp(std::chrono::steady_clock::now()), level(lvl), message(std::move(msg)), formatted(std::move(fmt)) {}

DebugOverlay *DebugOverlay::GetInstance() {
    if (!s_instance) {
        s_instance = new DebugOverlay();
    }
    return s_instance;
}

void DebugOverlay::PushLog(LogEntry::Level level, const std::string &message, const std::string &formatted) {
    std::lock_guard<std::mutex> lock(m_queueMutex);
    m_incomingQueue.emplace_back(level, message, formatted);
}

void DebugOverlay::Update() {
    ProcessQueue();
}

void DebugOverlay::ProcessQueue() {
    std::lock_guard<std::mutex> lock(m_queueMutex);

    while (!m_incomingQueue.empty()) {
        m_displayList.push_back(std::move(m_incomingQueue.front()));
        m_incomingQueue.pop_front();

        // 限制显示数量
        while (m_displayList.size() > m_maxLines) {
            m_displayList.pop_front();
        }
    }
}

void DebugOverlay::Render() {
    // 这里渲染到屏幕
    // 由于项目目前是纯 Win32，没有 ImGui
    // 这个方法将由 Window 或 Renderer 在主循环中调用
    // 目前先留空，后续可以扩展为 Win32 窗口或 ImGui 面板
}

void DebugOverlay::Clear() {
    std::lock_guard<std::mutex> lock(m_queueMutex);
    m_incomingQueue.clear();
    m_displayList.clear();
}

} // namespace Core
} // namespace DX12Engine
