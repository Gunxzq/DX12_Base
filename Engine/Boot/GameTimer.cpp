#include "GameTimer.h"

namespace DX12Engine {
namespace Core {

GameTimer::GameTimer() { ResetGameTime(); }

void GameTimer::Tick() {
    // 从 HighResolutionTimer 获取原始 delta
    m_rawDelta = m_highResTimer.Tick();

    // 应用暂停和缩放
    if (m_paused) {
        m_deltaTime = 0.0f;
    } else {
        m_deltaTime = m_rawDelta * m_timeScale;
        m_gameTime += m_deltaTime;
    }
}

void GameTimer::Pause() { m_paused = true; }

void GameTimer::Resume() { m_paused = false; }

void GameTimer::SetTimeScale(float scale) {
    if (scale < 0.0f) {
        scale = 0.0f;
    }
    m_timeScale = scale;
}

void GameTimer::ResetGameTime() {
    m_gameTime = 0.0f;
    m_deltaTime = 0.0f;
    m_rawDelta = 0.0f;
    m_highResTimer.Reset();
}

} // namespace Core
} // namespace DX12Engine