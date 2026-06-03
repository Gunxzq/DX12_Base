#pragma once
#include <DirectXMath.h>
#include <cstdint>

namespace DX12Engine::Renderer {

// 光源
struct Light {
    DirectX::XMFLOAT4 Strength;
    DirectX::XMFLOAT4 Direction;
    DirectX::XMFLOAT4 Position;
    float FalloffStart;
    float FalloffEnd;
    float SpotPower;
    float Range;
    int ShadowMapIndex;
    float Pad[3];
};

struct LightConstants {
    Light Lights[256];
    DirectX::XMFLOAT4 AmbientLight;
    uint32_t NumDirLights;
    uint32_t NumPointLights;
    uint32_t NumSpotLights;
    float Pad[5];
};

// -------------------------------------------------------------------

// 阴影对象常量
struct ShadowObjectConstants {
    DirectX::XMFLOAT4X4 World; // 被投射阴影的物体世界矩阵
};

// 方向光阴影常量
struct DirLightShadowConstants {
    DirectX::XMFLOAT4X4 LightViewProj; // 正交 VP 矩阵
    float ShadowMapSize;
    float Bias;
    float NormalBias;
    float ShadowStrength;
    uint32_t ShadowMapIndex;
    float Pad[3];
};

// 点光源阴影常量（立方体阴影）
struct PointLightShadowConstants {
    DirectX::XMFLOAT4X4 LightViewProj[6]; // 6 个面的 VP 矩阵
    DirectX::XMFLOAT3 LightPosition;
    float ShadowMapSize;
    float Bias;
    float NormalBias;
    float ShadowStrength;
    float Range;             // 光源范围
    uint32_t ShadowMapIndex; // 立方体贴图索引
    float Pad[2];
};

// 聚光源阴影常量
struct SpotLightShadowConstants {
    DirectX::XMFLOAT4X4 LightViewProj; // 透视 VP 矩阵
    float ShadowMapSize;
    float Bias;
    float NormalBias;
    float ShadowStrength;
    float SpotPower; // 聚光锥角
    uint32_t ShadowMapIndex;
    float Pad[2];
};
} // namespace DX12Engine::Renderer