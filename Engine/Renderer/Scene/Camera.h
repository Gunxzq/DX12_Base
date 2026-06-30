#pragma once

#include <DirectXMath.h>
#include <cstdint>

using namespace DirectX;

// 投影类型枚举
enum class ProjectionType : uint8_t {
    Perspective, // 透视投影（3D 游戏常用）
    Orthographic // 正交投影（UI、2D、编辑器常用）
};

// 相机结构体：纯数据容器 (POD)
struct Camera {
    // 显式声明默认构造函数
    Camera() = default;

    ProjectionType Type = ProjectionType::Perspective;

    // 透视参数
    float FOV = XMConvertToRadians(60.0f); // 垂直视场角
    float AspectRatio = 16.0f / 9.0f;

    // 正交参数
    float OrthoSize = 10.0f; // 正交视图的高度一半

    // 通用裁剪面
    float NearPlane = 0.5f;
    float FarPlane = 100.0f;

    XMFLOAT3 Position = {0.0f, 0.0f, 0.0f};
    XMFLOAT3 Rotation = {0.0f, 0.0f, 0.0f}; // Pitch, Yaw, Roll (弧度)

    // 相机运动预测（用于剔除延迟补偿）
    XMFLOAT3 Velocity = {0.0f, 0.0f, 0.0f};      // 当前帧速度（米/秒）
    XMFLOAT3 PrevPosition = {0.0f, 0.0f, 0.0f}; // 上一帧位置

    // 相机基向量（龙书风格，手动维护，避免欧拉角万向节死锁）
    XMFLOAT3 Forward = {0.0f, 0.0f, 1.0f}; // Look direction
    XMFLOAT3 Up = {0.0f, 1.0f, 0.0f};      // Up direction
    XMFLOAT3 Right = {1.0f, 0.0f, 0.0f};   // Right direction

    XMMATRIX ViewMatrix;
    XMMATRIX ProjMatrix;
    XMMATRIX ViewProjMatrix;
    XMMATRIX InverseView;
    XMMATRIX InverseProj;
    XMMATRIX InverseViewProj;
    XMMATRIX PrevViewProjMatrix;

    struct {
        bool bFollowTarget : 1;   // 是否跟随某个实体（第三人称）
        bool bLockPitch : 1;      // 是否锁定俯仰角（FPS 常用）
        bool bSmoothMovement : 1; // 是否启用位置平滑插值
    } Flags;

    XMFLOAT3 TargetOffset = {0.0f, 2.0f, -5.0f};
};