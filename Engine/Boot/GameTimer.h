
#ifndef GAMETIMER_H
#define GAMETIMER_H

#include "HighResolutionTimer.h"

namespace DX12Engine {
namespace Core {

/**
 * @brief 游戏逻辑计时器
 *
 * 职责：
 * - 管理游戏时间状态（暂停、缩放等）
 * - 内部使用 HighResolutionTimer 作为唯一高精度时间数据源
 * - 提供经过处理的游戏时间（DeltaTime、TotalTime）
 *
 * 与 HighResolutionTimer 的关系：
 * - HighResolutionTimer: 纯粹的高精度计时器，只提供原始时间数据
 * - GameTimer: 游戏层计时器，基于 HighResolutionTimer 添加游戏逻辑（暂停、缩放等）
 */
class GameTimer {
public:
    GameTimer();

    /**
     * @brief 每帧调用，更新时间状态
     * @note 必须在每帧开始时调用
     */
    void Tick();

    /**
     * @brief 暂停游戏时间
     * @note 暂停后 DeltaTime() 返回 0，但 GetRawDeltaTime() 仍返回真实时间
     */
    void Pause();

    /**
     * @brief 恢复游戏时间
     */
    void Resume();

    /**
     * @brief 设置时间缩放倍率
     * @param scale 时间缩放倍率 (0.0 = 暂停, 1.0 = 正常, 2.0 = 双倍速)
     */
    void SetTimeScale(float scale);

    /**
     * @brief 获取当前时间缩放倍率
     */
    float GetTimeScale() const { return m_timeScale; }

    /**
     * @brief 检查是否暂停
     */
    bool IsPaused() const { return m_paused; }

    /**
     * @brief 重置累计游戏时间（如新关卡开始时）
     * @note 同时会重置底层 HighResolutionTimer
     */
    void ResetGameTime();

    // ========================================================================
    // 获取时间接口
    // ========================================================================

    /**
     * @brief 获取应用缩放后的游戏帧间隔（秒）
     * @return 如果暂停则返回 0，否则返回 rawDelta * timeScale
     */
    float GetDeltaTime() const { return m_deltaTime; }

    /**
     * @brief 获取累计游戏时间（秒）
     * @return 受暂停和缩放影响的累计时间
     */
    float GetGameTime() const { return m_gameTime; }

    /**
     * @brief 获取原始真实帧间隔（秒）
     * @return 不受暂停和缩放影响的真实时间
     */
    float GetRawDeltaTime() const { return m_rawDelta; }

    /**
     * @brief 获取从程序启动的累计真实时间（秒）
     * @return 不受任何游戏状态影响的真实时间
     */
    float GetRawTotalTime() const { return m_highResTimer.TotalTime(); }

private:
    /** @brief 底层高精度计时器（唯一时间数据源） */
    HighResolutionTimer m_highResTimer;

    /** @brief 时间缩放倍率 (1.0 = 正常, 0.5 = 半速, 0.0 = 暂停) */
    float m_timeScale = 1.0f;

    /** @brief 是否暂停 */
    bool m_paused = false;

    /** @brief 原始帧间隔（秒），来自 HighResolutionTimer */
    float m_rawDelta = 0.0f;

    /** @brief 应用缩放后的游戏帧间隔（秒） */
    float m_deltaTime = 0.0f;

    /** @brief 累计游戏时间（秒） */
    float m_gameTime = 0.0f;
};

} // namespace Core
} // namespace DX12Engine

#endif // GAMETIMER_H