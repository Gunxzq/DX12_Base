#pragma once
#include <cmath>
#include <DirectXMath.h>

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

struct FVector3D {
    float X = 0.0f;
    float Y = 0.0f;
    float Z = 0.0f;

    FVector3D() = default;
    FVector3D(float x, float y, float z) : X(x), Y(y), Z(z) {}
    explicit FVector3D(const DirectX::XMFLOAT3 &v) : X(v.x), Y(v.y), Z(v.z) {}

    DirectX::XMFLOAT3 ToXMFLOAT3() const { return {X, Y, Z}; }
    DirectX::XMVECTOR ToXMVECTOR() const { return DirectX::XMVectorSet(X, Y, Z, 0.0f); }

    FVector3D operator+(const FVector3D &other) const { return {X + other.X, Y + other.Y, Z + other.Z}; }
    FVector3D operator-(const FVector3D &other) const { return {X - other.X, Y - other.Y, Z - other.Z}; }
    FVector3D operator*(float scalar) const { return {X * scalar, Y * scalar, Z * scalar}; }
    FVector3D operator/(float scalar) const { return {X / scalar, Y / scalar, Z / scalar}; }

    FVector3D &operator+=(const FVector3D &other) {
        X += other.X; Y += other.Y; Z += other.Z;
        return *this;
    }

    float Length() const { return std::sqrt(X * X + Y * Y + Z * Z); }
    float LengthSquared() const { return X * X + Y * Y + Z * Z; }

    FVector3D Normalized() const {
        float len = Length();
        return (len > 0.0001f) ? FVector3D{X / len, Y / len, Z / len} : FVector3D{};
    }

    void Normalize() {
        float len = Length();
        if (len > 0.0001f) { X /= len; Y /= len; Z /= len; }
    }

    static float Dot(const FVector3D &a, const FVector3D &b) { return a.X * b.X + a.Y * b.Y + a.Z * b.Z; }
    static FVector3D Cross(const FVector3D &a, const FVector3D &b) {
        return {a.Y * b.Z - a.Z * b.Y, a.Z * b.X - a.X * b.Z, a.X * b.Y - a.Y * b.X};
    }
};

// ============================================================================
// 射线结构体
// ============================================================================
struct FRay {
    FVector3D Origin;
    FVector3D Direction; // 归一化方向
};

} // namespace DX12Engine