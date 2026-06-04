// ShadowSampling.hlsl
// 阴影采样函数库

#pragma enable_unbounded_descriptor_tables

#ifndef SHADOW_SAMPLING_HLSL
#define SHADOW_SAMPLING_HLSL

// ============================================================================
// 阴影常量（从 CPU 上传的 StructuredBuffer）
// ============================================================================

struct DirShadowData
{
    row_major float4x4 LightViewProj; // 与 CPU DirLightShadowConstants row-major 布局一致
    float ShadowMapSize;
    float Bias;
    float NormalBias;
    float ShadowStrength;
    uint ShadowMapIndex;
    float Pad[3];
};

struct PointShadowData
{
    row_major float4x4 LightViewProj[6];
    float3 LightPosition;
    float ShadowMapSize;
    float Bias;
    float NormalBias;
    float ShadowStrength;
    float Range;
    uint ShadowMapIndex;
    float Pad[2];
};

struct SpotShadowData
{
    row_major float4x4 LightViewProj;
    float ShadowMapSize;
    float Bias;
    float NormalBias;
    float ShadowStrength;
    float SpotPower;
    uint ShadowMapIndex;
    float Pad[2];
};

// ============================================================================
// 资源绑定
// ============================================================================

StructuredBuffer<DirShadowData> gDirShadows : register(t11, space1);
StructuredBuffer<PointShadowData> gPointShadows : register(t12, space1);
StructuredBuffer<SpotShadowData> gSpotShadows : register(t13, space1);

Texture2D gDirShadowMaps[] : register(t14, space1);
TextureCubeArray gPointShadowMaps : register(t20, space1);
Texture2D gSpotShadowMaps[] : register(t26, space1);

// ============================================================================
// 辅助函数
// ============================================================================

// 计算阴影贴图 UV 和深度比较值
float2 ComputeShadowUV(float4 shadowPos)
{
    // 透视除法
    float3 proj = shadowPos.xyz / shadowPos.w;
    // 从 [-1,1] 映射到 [0,1]
    float2 uv = float2(proj.x * 0.5f + 0.5f, 1.0f - (proj.y * 0.5f + 0.5f));
    return uv;
}

// 方向光阴影采样
float SampleDirShadow(uint shadowIdx, float3 worldPos, float3 normal, float3 lightDir)
{
    DirShadowData shadow = gDirShadows[shadowIdx];

    // 法线偏移（在变换前应用）
    float3 offsetWorldPos = worldPos + lightDir * shadow.NormalBias;
    float4 shadowPos = mul(float4(offsetWorldPos, 1.0f), shadow.LightViewProj);

    // 透视除法
    float3 projCoords = shadowPos.xyz / shadowPos.w;

    // 超出阴影范围检查（使用归一化后的坐标）
    if (projCoords.z < 0.0f || projCoords.z > 1.0f)
    {
        return 1.0f;
    }

    // UV 坐标：从 [-1,1] 映射到 [0,1]，且翻转 Y（因为 DX 纹理坐标原点在左上角）
    float2 uv = ComputeShadowUV(shadowPos);

    // 边界检查
    if (uv.x < 0.0f || uv.x > 1.0f || uv.y < 0.0f || uv.y > 1.0f)
    {
        return 1.0f;
    }

    float compareDepth = projCoords.z;

    // PCF 采样
    float shadowFactor = 0.0f;
    const float texelSize = 1.0f / shadow.ShadowMapSize;

    for (int x = -1; x <= 1; ++x)
    {
        for (int y = -1; y <= 1; ++y)
        {
            float2 offsetUV = uv + float2(x, y) * texelSize;
            shadowFactor += gDirShadowMaps[shadow.ShadowMapIndex].SampleCmpLevelZero(
                gShadowSampler, offsetUV, compareDepth);
        }
    }

    shadowFactor /= 9.0f;
    return lerp(1.0f, shadowFactor, shadow.ShadowStrength);
}

// // 点光源阴影采样（立方体贴图）
// float SamplePointShadow(uint shadowIdx, float3 worldPos, float3 lightPos, float range)
// {
//     PointShadowData shadow = gPointShadows[shadowIdx];

//     float3 L = worldPos - lightPos;
//     float distance = length(L);

//     if (distance >= range)
//     {
//         return 1.0f;
//     }

//     float3 direction = normalize(L);

//     // 采样立方体贴图
//     float shadowDepth = gPointShadowMaps.SampleLevel(gSamplerLinearClamp, float4(direction, cubeIndex), 0.0f).r;
//     // 线性化深度比较
//     float compareDepth = distance / range;

//     float shadowFactor = (compareDepth - shadow.Bias <= shadowDepth) ? 1.0f : 0.0f;
//     return lerp(1.0f, shadowFactor, shadow.ShadowStrength);
// }

// 聚光灯阴影采样
float SampleSpotShadow(uint shadowIdx, float3 worldPos, float3 normal, float3 lightDir)
{
    SpotShadowData shadow = gSpotShadows[shadowIdx];

    // 变换到光源裁剪空间
    float4 shadowPos = mul(float4(worldPos, 1.0f), shadow.LightViewProj);

    if (shadowPos.z < 0.0f || shadowPos.z > 1.0f)
    {
        return 1.0f;
    }

    float2 uv = ComputeShadowUV(shadowPos);

    if (uv.x < 0.0f || uv.x > 1.0f || uv.y < 0.0f || uv.y > 1.0f)
    {
        return 1.0f;
    }

    // 法线偏移
    float3 offset = normal * shadow.NormalBias;
    shadowPos = mul(float4(worldPos + offset, 1.0f), shadow.LightViewProj);
    uv = ComputeShadowUV(shadowPos);

    float compareDepth = shadowPos.z;

    float shadowFactor = 0.0f;
    const float texelSize = 1.0f / shadow.ShadowMapSize;

    for (int x = -1; x <= 1; ++x)
    {
        for (int y = -1; y <= 1; ++y)
        {
            float2 offsetUV = uv + float2(x, y) * texelSize;
            shadowFactor += gSpotShadowMaps[shadow.ShadowMapIndex].SampleCmpLevelZero(
                gShadowSampler, offsetUV, compareDepth);
        }
    }

    shadowFactor /= 9.0f;
    return lerp(1.0f, shadowFactor, shadow.ShadowStrength);
}

#endif // SHADOW_SAMPLING_HLSL