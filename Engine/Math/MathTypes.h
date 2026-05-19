#pragma once
#include <cmath>

namespace DX12Engine {

struct FVector2D {
    float X = 0.0f;
    float Y = 0.0f;

    FVector2D() = default;
    FVector2D(float x, float y) : X(x), Y(y) {}

    // 常用操作
    FVector2D operator+(const FVector2D &other) const { return {X + other.X, Y + other.Y}; }
    FVector2D operator-(const FVector2D &other) const { return {X - other.X, Y - other.Y}; }
    FVector2D operator*(float scalar) const { return {X * scalar, Y * scalar}; }

    float Length() const { return std::sqrt(X * X + Y * Y); }
    float LengthSquared() const { return X * X + Y * Y; }

    void Normalize() {
        float len = Length();
        if (len > 0.0001f) {
            X /= len;
            Y /= len;
        }
    }

    static float Dot(const FVector2D &a, const FVector2D &b) { return a.X * b.X + a.Y * b.Y; }
};

} // namespace DX12Engine