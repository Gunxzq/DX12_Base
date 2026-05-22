#pragma once
#include <cstdint>
#include <vector>

namespace DX12Engine {
namespace Input {

enum class EActionValueType : uint8_t {
    Digital,  // 布尔值
    Analog1D, // 单轴浮点值
    Axis2D,   // 二维轴值
    Toggle,   // 开关状态
    Charge    // 蓄力值
};

struct InputActionState {
    EActionValueType Type = EActionValueType::Digital;

    bool bActive = false; // 是否有输入（统一判断，用于边缘检测）

    struct DigitalState {
        bool bPressed : 1;       // 刚按下（单帧）
        bool bReleased : 1;      // 刚抬起（单帧）
        bool bHeld : 1;          // 持续按住
        bool bTapped : 1;        // 短按（按下并快速释放）
        bool bDoubleTapped : 1;  // 双击
        bool bLongPressed : 1;   // 长按触发（单帧）
        bool bHoldRelease : 1;   // 长按释放触发（单帧）
        bool bRepeatTrigger : 1; // 重复触发（单帧）
        uint8_t Reserved : 8;    // 预留位
    } Digital;

    union {
        float Value1D; // Analog1D: -1.0 ~ 1.0 或 0.0 ~ 1.0
        struct {
            float X;
            float Y;
        } Axis2D;          // Axis2D: (-1.0~1.0, -1.0~1.0)
        float ChargeValue; // Hold 蓄力值: 0.0 ~ 1.0
    };

    bool bToggleState = false; // Toggle 当前状态（切换型动作）

    float PressStartTime = 0.0f;  // 当前按下开始的时间
    float LastReleaseTime = 0.0f; // 上次释放的时间
    float HoldDuration = 0.0f;    // 当前已按住的时间
    float RepeatTimer = 0.0f;     // 重复触发计时器
    float RepeatInterval = 0.0f;  // 重复触发间隔（秒）

    std::vector<uint64_t> SequenceBuffer; // 序列按键哈希值缓冲
    uint32_t SequenceProgress = 0;        // 当前序列进度

    InputActionState()
        : bActive(false), Digital{false, false, false, false, false, false, false, false, 0}, Value1D(0.0f),
          bToggleState(false), PressStartTime(0.0f), LastReleaseTime(0.0f), HoldDuration(0.0f), RepeatTimer(0.0f),
          RepeatInterval(0.0f) {}

    void SetDigital(bool pressed, bool released, bool held, bool tapped = false, bool doubleTapped = false,
                    bool longPressed = false, bool holdRelease = false, bool repeatTrigger = false) {
        Type = EActionValueType::Digital;
        Digital.bPressed = pressed;
        Digital.bReleased = released;
        Digital.bHeld = held;
        Digital.bTapped = tapped;
        Digital.bDoubleTapped = doubleTapped;
        Digital.bLongPressed = longPressed;
        Digital.bHoldRelease = holdRelease;
        Digital.bRepeatTrigger = repeatTrigger;
        bActive = held;
    }

    void SetAnalog1D(float value) {
        Type = EActionValueType::Analog1D;
        Value1D = value;
        bActive = (std::abs(value) > 0.001f);
    }

    void SetAxis2D(float x, float y) {
        Type = EActionValueType::Axis2D;
        Axis2D.X = x;
        Axis2D.Y = y;
        bActive = (std::abs(x) > 0.001f || std::abs(y) > 0.001f);
    }

    void SetCharge(float value) {
        Type = EActionValueType::Charge;
        ChargeValue = value;
        bActive = (value > 0.001f);
    }

    void SetToggle(bool state) {
        Type = EActionValueType::Toggle;
        bToggleState = state;
        bActive = state; // Toggle 的活跃状态就是其开关状态
    }

    void Toggle() {
        Type = EActionValueType::Toggle;
        bToggleState = !bToggleState;
        bActive = bToggleState;
    }

    void SetActive(bool active) { bActive = active; }

    bool GetPressed() const { return Type == EActionValueType::Digital ? Digital.bPressed : false; }
    bool GetReleased() const { return Type == EActionValueType::Digital ? Digital.bReleased : false; }
    bool GetHeld() const { return Type == EActionValueType::Digital ? Digital.bHeld : false; }
    bool GetTapped() const { return Type == EActionValueType::Digital ? Digital.bTapped : false; }
    bool GetDoubleTapped() const { return Type == EActionValueType::Digital ? Digital.bDoubleTapped : false; }
    bool GetLongPressed() const { return Type == EActionValueType::Digital ? Digital.bLongPressed : false; }
    bool GetHoldRelease() const { return Type == EActionValueType::Digital ? Digital.bHoldRelease : false; }
    bool GetRepeatTrigger() const { return Type == EActionValueType::Digital ? Digital.bRepeatTrigger : false; }

    float GetValue1D() const { return Type == EActionValueType::Analog1D ? Value1D : 0.0f; }

    void GetAxis2D(float &outX, float &outY) const {
        if (Type == EActionValueType::Axis2D) {
            outX = Axis2D.X;
            outY = Axis2D.Y;
        } else {
            outX = 0.0f;
            outY = 0.0f;
        }
    }

    float GetCharge() const { return Type == EActionValueType::Charge ? ChargeValue : 0.0f; }

    bool GetToggleState() const { return Type == EActionValueType::Toggle ? bToggleState : false; }

    bool IsActive() const { return bActive; }

    void ResetFrameState() {
        if (Type == EActionValueType::Digital) {
            Digital.bPressed = false;
            Digital.bReleased = false;
            Digital.bTapped = false;
            Digital.bDoubleTapped = false;
            Digital.bLongPressed = false;
            Digital.bHoldRelease = false;
            Digital.bRepeatTrigger = false;
            // bHeld 保持不变（由持续输入维护）
            // bActive 保持不变（由 bHeld 或轴值维护）
        }
        // 蓄力值不清零，由逻辑决定何时重置
        // Toggle 状态不清零
        RepeatTimer = 0.0f;
    }

    /// @brief 重置所有状态（窗口失焦时调用）
    void ResetAll() {
        bActive = false;
        Digital = {false, false, false, false, false, false, false, false, 0};
        Value1D = 0.0f;
        bToggleState = false;
        PressStartTime = 0.0f;
        LastReleaseTime = 0.0f;
        HoldDuration = 0.0f;
        RepeatTimer = 0.0f;
        RepeatInterval = 0.0f;
        SequenceBuffer.clear();
        SequenceProgress = 0;
    }

    /// @brief 序列检测：添加按键
    void AddToSequence(uint64_t hashedKey) {
        SequenceBuffer.push_back(hashedKey);
        if (SequenceBuffer.size() > 32) {
            SequenceBuffer.erase(SequenceBuffer.begin());
        }
    }

    /// @brief 检查序列是否匹配
    bool IsSequenceMatch(const std::vector<uint64_t> &target) const {
        if (SequenceBuffer.size() < target.size())
            return false;
        for (size_t i = 0; i < target.size(); ++i) {
            size_t idx = SequenceBuffer.size() - target.size() + i;
            if (idx >= SequenceBuffer.size())
                return false;
            if (SequenceBuffer[idx] != target[i])
                return false;
        }
        return true;
    }
};

} // namespace Input
} // namespace DX12Engine