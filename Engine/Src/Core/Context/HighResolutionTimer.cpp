#include "Core/Context/HighResolutionTimer.h"

namespace DX12Engine {
namespace Core {
HighResolutionTimer::HighResolutionTimer() {
    QueryPerformanceFrequency((LARGE_INTEGER *)&m_frequency);
    Reset();
}

void HighResolutionTimer::Reset() {
    QueryPerformanceCounter((LARGE_INTEGER *)&m_startTime);
    m_prevTime = m_startTime;
}

float HighResolutionTimer::Tick() {
    __int64 current;
    QueryPerformanceCounter((LARGE_INTEGER *)&current);

    float delta = static_cast<float>(current - m_prevTime) / m_frequency;

    // 限制最大值（防止调试时断点导致巨大delta）
    if (delta > 0.1f)
        delta = 0.1f;
    if (delta < 0.0f)
        delta = 0.0f;

    m_prevTime = current;
    return delta;
}

float HighResolutionTimer::TotalTime() const {
    __int64 current;
    QueryPerformanceCounter((LARGE_INTEGER *)&current);
    return static_cast<float>(current - m_startTime) / m_frequency;
}

} // namespace Core
} // namespace DX12Engine