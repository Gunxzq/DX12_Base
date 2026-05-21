#include "GameContext.h"
#include "ConfigManager.h"
#include "Logger/Logger.h"
#include "Platform/Input/InputSystem.h"
#include "Platform/Windows/Window.h"
#include "Renderer/RHI/Command/CommandManager.h"
#include "Renderer/RHI/D3D12DeviceContext.h"

namespace DX12Engine::Boot {

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

} // namespace DX12Engine::Boot
