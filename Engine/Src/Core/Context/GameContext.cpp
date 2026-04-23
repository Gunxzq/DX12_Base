#include "Core/Context/GameContext.h"
#include "Core/Config/ConfigManager.h"
#include "Renderer/Core/D3D12DeviceContext.h"
#include "System/Logger/Logger.h"
#include "System/Window/Window.h"

namespace DX12Engine {
namespace Core {

bool GameContext::IsValid() const {
    if (!Window) {
        m_invalidReason = "Window is not set";
        return false;
    }
    if (!Config) {
        m_invalidReason = "Config is not set";
        return false;
    }
    if (!Logging) {
        m_invalidReason = "Logging is not set";
        return false;
    }
    if (!MainTimer) {
        m_invalidReason = "MainTimer is not set";
        return false;
    }
    if (!DeviceContext) {
        m_invalidReason = "DeviceContext is not set";
        return false;
    }
    m_invalidReason = nullptr;
    return true;
}

const char *GameContext::GetInvalidReason() const { return m_invalidReason ? m_invalidReason : "All fields are valid"; }

} // namespace Core
} // namespace DX12Engine
