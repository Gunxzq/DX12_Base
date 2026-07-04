#pragma once
#include <DirectXMath.h>
#include <cstdint>

#pragma pack(push, 16)

namespace DX12Engine::Renderer {

// 光源 (必须与 LightingUtil.hlsl 中 Light 结构体严格对齐)
struct Light {
    DirectX::XMFLOAT4 Strength;  // offset 0
    DirectX::XMFLOAT4 Direction; // offset 16
    DirectX::XMFLOAT4 Position;  // offset 32
    float FalloffStart;          // offset 48
    float FalloffEnd;            // offset 52
    float SpotPower;             // offset 56
    float Range;                 // offset 60
    float CastShadow;            // offset 64
    float ShadowBias;            // offset 68
    float ShadowMapIndex;        // offset 72
    float Type;                  // offset 76: 0=Directional, 1=Point, 2=Spot
};

static_assert(sizeof(Light) % 16 == 0, "Light size mismatch");

struct LightConstants {
    Light Lights[256];
    DirectX::XMFLOAT4 AmbientLight;
    uint32_t NumDirLights;
    uint32_t NumPointLights;
    uint32_t NumSpotLights;
    float Pad[5];
};

static_assert(sizeof(LightConstants) % 16 == 0, "LightConstants size mismatch");

// -------------------------------------------------------------------

// 阴影对象常量
struct ShadowObjectConstants {
    DirectX::XMFLOAT4X4 World; // 被投射阴影的物体世界矩阵
};

static_assert(sizeof(ShadowObjectConstants) % 16 == 0, "ShadowObjectConstants size mismatch");

// 统一阴影采样参数（与 ShadowSampling.hlsl ShadowParams 严格对齐）
struct ShadowParams {
    uint32_t Type;                     // 0=Directional, 1=Point
    uint32_t ShadowMapIndex;           // gShadowMaps[] 纹理索引
    float ShadowMapSize;               // PCF 纹素步长
    float ShadowStrength;              // 阴影强度
    float Bias;                        // 深度偏移
    float NormalBias;                  // 法线偏移（点光源=0）
    float pad1[2];                     // 对齐到 16 字节
    DirectX::XMFLOAT3 LightPosition;   // 点光源位置（方向光=0）
    float Range;                       // 点光源衰减范围（方向光=0）
    DirectX::XMFLOAT4X4 LightViewProj; // 方向光 VP 矩阵（点光源填 0）
};

static_assert(sizeof(ShadowParams) % 16 == 0, "ShadowParams size mismatch");
static_assert(sizeof(ShadowParams) == 112, "ShadowParams size must be 112 bytes");

// 方向光阴影常量（供 ShadowRenderer cbuffer b1 使用，布局与 Shadow.hlsl cbDirShadow 严格对齐）
struct DirLightShadowConstants {
    DirectX::XMFLOAT4X4 LightViewProj; // 正交 VP 矩阵
    float ShadowMapSize;
    float Bias;
    float NormalBias;
    float ShadowStrength;
    uint32_t ShadowMapIndex;
    float Pad[3];
};

static_assert(sizeof(DirLightShadowConstants) % 16 == 0, "DirLightShadowConstants size mismatch");

// 点光源阴影常量（立方体阴影，供阴影渲染 Pass 使用，非采样）
struct PointLightShadowConstants {
    DirectX::XMFLOAT4X4 LightViewProj[6]; // 6 个面的 VP 矩阵
    DirectX::XMFLOAT4 LightPosition;
    float ShadowMapSize;
    float Bias;
    float NormalBias;
    float ShadowStrength;
    float Range;             // 光源范围
    uint32_t ShadowMapIndex; // 立方体贴图索引
    float Pad[2];
};

static_assert(sizeof(PointLightShadowConstants) % 16 == 0, "PointLightShadowConstants size mismatch");

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

static_assert(sizeof(SpotLightShadowConstants) % 16 == 0, "SpotLightShadowConstants size mismatch");
} // namespace DX12Engine::Renderer

#pragma pack(pop)
