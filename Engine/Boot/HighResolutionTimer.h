#pragma once

#include "Common/WindowsPlatform.h"

namespace DX12Engine {
namespace Boot {

/**
 * @brief 纯粹的高精度计时器
 *
 * 职责：只测量真实世界的时间流逝
 * - 不涉及暂停、时间缩放、累计游戏时间等逻辑
 * - 只提供原始时间数据
 *
 * 使用方式：
 *   timer.Reset();
 *   while (running) {
 *       float delta = timer.Tick();
 *       // 使用 delta...
 *   }
 */
class HighResolutionTimer {
public:
    HighResolutionTimer();

    /**
     * @brief 重置计时器（设置起始点）
     * 通常在游戏启动或关卡开始时调用
     */
    void Reset();

    /**
     * @brief 每帧调用，返回从上一帧到现在的真实时间间隔（秒）
     * @return 帧间隔（秒），最大值被限制为 0.1 秒
     */
    float Tick();

    /**
     * @brief 获取从 Reset() 调用开始的总真实时间（秒）
     * @return 累计时间（秒）
     */
    float TotalTime() const;

private:
    __int64 m_frequency; // 每秒计数次数
    __int64 m_startTime; // Reset() 时的计数
    __int64 m_prevTime;  // 上一帧的计数
};

} // namespace Boot
} // namespace DX12Engine