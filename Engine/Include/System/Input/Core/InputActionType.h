#pragma once

namespace DX12Engine {
namespace Input {

/**
 * @brief 输入行为类型枚举
 * 定义了输入系统如何解释原始硬件信号
 */
enum class EInputActionType {
    // --- 基础类型 ---

    Digital, // Pressed, Released, Held

    /**
     * @brief 一维模拟型 (Analog 1D)
     * 值域: -1.0 ~ 1.0 (或 0.0 ~ 1.0)
     * 示例: MoveForward, Throttle, Volume
     */
    Analog1D, // MoveForward, Throttle, Volume

    /**
     * @brief 二维轴向型 (Axis 2D)
     * 值域: FVector2D (-1.0 ~ 1.0)
     * 示例: Move (WASD), Look (Right Stick)
     */
    Axis2D,

    // --- 高级类型 ---

    /**
     * @brief 组合键型 (Chord)
     * 需要主键 + 修饰键同时满足
     * 示例: Sprint (Shift + W), CrouchWalk (Ctrl + W)
     */
    Chord,

    /**
     * @brief 短按型 (Tap)
     * 快速按下并释放，超过时间阈值视为无效或转为 Hold
     * 示例: LightAttack, DoubleJump (first tap)
     */
    Tap,

    /**
     * @brief 长按/蓄力型 (Hold)
     * 按下期间 Value 随时间增加，释放时触发最终效果
     * 示例: ChargeShot, BowPull
     */
    Hold,

    /**
     * @brief 切换型 (Toggle)
     * 每次触发翻转内部布尔状态
     * 示例: ToggleFlashlight, ToggleAutoRun
     */
    Toggle,

    DoubleTap,   // 双击（窗口时间内两次按下）
    HoldRelease, // 长按释放时触发（Value 持续，释放时触发）
    Repeat,      // 重复触发（按住时每帧触发）
    Sequence     // 按键序列（上上下下左右左右BA）
};

} // namespace Input
} // namespace DX12Engine