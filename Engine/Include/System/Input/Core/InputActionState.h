#pragma once
#include <cstdint>

namespace DX12Engine {
namespace Input {

enum class EActionValueType : uint8_t { Digital, Analog1D, Axis2D };

struct InputActionState {
    EActionValueType Type = EActionValueType::Digital;

    union {
        struct {
            bool bPressed : 1;
            bool bReleased : 1;
            bool bHeld : 1;
            bool bTapped : 1;       // 引擎层计算：短按
            bool bDoubleTapped : 1; // 引擎层计算：双击
            uint8_t Reserved : 3;
        } Digital;

        float Value1D;

        struct {
            float X;
            float Y;
        } Axis2D;
    };

    InputActionState() { SetDigital(false, false, false, false, false); }

    // --- Setters ---
    void SetDigital(bool pressed, bool released, bool held, bool tapped = false, bool doubleTapped = false) {
        Type = EActionValueType::Digital;
        Digital.bPressed = pressed;
        Digital.bReleased = released;
        Digital.bHeld = held;
        Digital.bTapped = tapped;
        Digital.bDoubleTapped = doubleTapped;
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
    bool IsDigital() const { return Type == EActionValueType::Digital; }
    bool IsAnalog1D() const { return Type == EActionValueType::Analog1D; }
    bool IsAxis2D() const { return Type == EActionValueType::Axis2D; }

    bool GetPressed() const { return IsDigital() ? Digital.bPressed : false; }
    bool GetReleased() const { return IsDigital() ? Digital.bReleased : false; }
    bool GetHeld() const { return IsDigital() ? Digital.bHeld : false; }
    bool GetTapped() const { return IsDigital() ? Digital.bTapped : false; }
    bool GetDoubleTapped() const { return IsDigital() ? Digital.bDoubleTapped : false; }

    float GetValue1D() const { return IsAnalog1D() ? Value1D : 0.0f; }

    void GetAxis2D(float &outX, float &outY) const {
        if (IsAxis2D()) {
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