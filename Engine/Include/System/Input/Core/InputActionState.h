#pragma once
#include <cstdint>

namespace DX12Engine {
namespace Input {

enum class EActionValueType : uint8_t { Digital, Analog1D, Axis2D };

struct InputActionState {
    EActionValueType Type = EActionValueType::Digital;

    // --- 数字状态 ---
    struct DigitalState {
        bool bPressed : 1;
        bool bReleased : 1;
        bool bHeld : 1;
        bool bTapped : 1;       // 短按（按下并快速释放）
        bool bDoubleTapped : 1; // 双击
        bool bLongPressed : 1;  // 长按触发
        uint8_t Reserved : 2;
    } Digital;

    // --- 模拟状态 ---
    union {
        float Value1D;
        struct {
            float X;
            float Y;
        } Axis2D;
    };

    // --- 时间追踪 (用于长按/双击) ---
    float PressStartTime = 0.0f;  // 当前按下开始的时间
    float LastReleaseTime = 0.0f; // 上次释放的时间
    float HoldDuration = 0.0f;    // 当前已按住的时间

    InputActionState()
        : Digital{false, false, false, false, false, false, 0}, Value1D(0.0f), PressStartTime(0.0f),
          LastReleaseTime(0.0f), HoldDuration(0.0f) {}

    // --- Setters ---
    void SetDigital(bool pressed, bool released, bool held, bool tapped = false, bool doubleTapped = false,
                    bool longPressed = false) {
        Type = EActionValueType::Digital;
        Digital.bPressed = pressed;
        Digital.bReleased = released;
        Digital.bHeld = held;
        Digital.bTapped = tapped;
        Digital.bDoubleTapped = doubleTapped;
        Digital.bLongPressed = longPressed;
        Digital.Reserved = 0;
    }

    void SetAnalog1D(float value) {
        Type = EActionValueType::Analog1D;
        Value1D = value;
    }

    void SetAxis2D(float x, float y) {
        Type = EActionValueType::Axis2D;
        Axis2D.X = x;
        Axis2D.Y = y;
    }

    // --- Getters ---
    bool GetPressed() const { return Type == EActionValueType::Digital ? Digital.bPressed : false; }
    bool GetReleased() const { return Type == EActionValueType::Digital ? Digital.bReleased : false; }
    bool GetHeld() const { return Type == EActionValueType::Digital ? Digital.bHeld : false; }
    bool GetTapped() const { return Type == EActionValueType::Digital ? Digital.bTapped : false; }
    bool GetDoubleTapped() const { return Type == EActionValueType::Digital ? Digital.bDoubleTapped : false; }
    bool GetLongPressed() const { return Type == EActionValueType::Digital ? Digital.bLongPressed : false; }

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
};

} // namespace Input
} // namespace DX12Engine