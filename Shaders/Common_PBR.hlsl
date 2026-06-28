#ifndef COMMON_PBR_HLSL
#define COMMON_PBR_HLSL

#include "LightingUtil.hlsl"

// =================================================================================================
// 材质数据 (对应 C++ MaterialConstants)
// =================================================================================================

struct MaterialData
{
    float4 BaseColor;
    float Metallic;
    float Roughness;
    float Ambient;
    float Alpha;
    float4 Emissive;
    float AlphaCutoff;
    float NormalStrength;

    // 贴图索引（0xFFFFFFFF = 无效）
    uint BaseColorTexIndex;
    uint NormalTexIndex;
    uint MetallicRoughnessTexIndex;
    uint EmissiveTexIndex;
    uint OcclusionTexIndex;
    uint HeightTexIndex;
    uint OpacityTexIndex;
    uint MaskTexIndex;
    uint SubsurfaceTexIndex;
    uint ClearCoatTexIndex;
};

// =================================================================================================
// 共用纹理堆（所有物体共享的无界纹理数组）
// =================================================================================================
Texture2D gTextureMaps[] : register(t0, space2);

// =================================================================================================
// 常量缓冲
// =================================================================================================

cbuffer cbPerObject : register(b0)
{
    row_major float4x4 gWorld;
    row_major float4x4 gWorldInvTrans;
    row_major float4x4 gPrevWorld;
    uint gMaterialIndex;
    uint gReceiveShadow;
    float2 gObjPad;
}

cbuffer cbPass : register(b1)
{
    row_major float4x4 gView;
    row_major float4x4 gProj;
    row_major float4x4 gViewProj;
    row_major float4x4 gInvView;
    row_major float4x4 gInvProj;
    row_major float4x4 gInvViewProj;
    row_major float4x4 gPrevViewProj;
    float3 gCameraPos;
    float gTotalTime;
    float gDeltaTime;
    float gNearPlane;
    float gFarPlane;
    float gAspectRatio;
    uint gFrameCount;
    float4 gPad[3];
}

cbuffer cbLights : register(b2)
{
    Light gLights[256];
    float4 gAmbientLight;
    uint gNumDirLights;
    uint gNumPointLights;
    uint gNumSpotLights;
    uint gLightsPad[5];
}

// =================================================================================================
// 资源绑定
// =================================================================================================

StructuredBuffer<MaterialData> gMaterialData : register(t0, space1);

#ifndef DISABLE_ENV_REFLECTION
TextureCube gEnvMap : register(t10);
SamplerState gEnvSampler : register(s10);
#endif

// 采样器
SamplerState gSamplerPointWrap : register(s0);
SamplerState gSamplerPointClamp : register(s1);
SamplerState gSamplerLinearWrap : register(s2);
SamplerState gSamplerLinearClamp : register(s3);
SamplerState gSamplerAnisotropicWrap : register(s4);
SamplerState gSamplerAnisotropicClamp : register(s5);
SamplerComparisonState gShadowSampler : register(s11);

// =================================================================================================
// 环境反射
// =================================================================================================

float3 ComputeEnvironmentReflection(float3 reflectDir, float3 albedo, float metallic, float roughness, float3 N, float3 V)
{
#ifndef DISABLE_ENV_REFLECTION
    float3 reflection = gEnvMap.Sample(gEnvSampler, reflectDir).rgb;
    float3 F0 = lerp(0.04f, albedo, metallic);
    float NdotV = max(dot(N, V), 0.0f);
    float3 fresnel = FresnelSchlick(NdotV, F0);
    float strength = 1.0f - roughness * 0.5f;
    return reflection * fresnel * strength;
#else
    return float3(0.0f, 0.0f, 0.0f);
#endif
}
#endif