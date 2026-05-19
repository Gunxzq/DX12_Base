#pragma once
#include <DirectXMath.h>

using namespace DirectX;

// 对应 HLSL cbPass : register(b1)
struct PassConstants {
    XMFLOAT4X4 View;
    XMFLOAT4X4 Proj;
    XMFLOAT4X4 ViewProj;
    XMFLOAT3 CameraPos;
    float TotalTime; // 游戏运行总时间
};

// 对应 HLSL cbPerObject : register(b0)
struct ObjectConstants {
    XMFLOAT4X4 World;
    XMFLOAT4X4 WorldInvTranspose;
};