// ShadowSampling.hlsl
// 阴影采样函数库

#pragma enable_unbounded_descriptor_tables

#ifndef SHADOW_SAMPLING_HLSL
#define SHADOW_SAMPLING_HLSL

// ============================================================================
// 阴影常量（从 CPU 上传的 StructuredBuffer）
// ============================================================================

struct ShadowParams
{
    uint Type;                        // 0=Directional, 1=Point (2=Spot 预留)
    uint ShadowMapIndex;              // gShadowMaps[] 纹理索引
    float ShadowMapSize;              // PCF 纹素步长
    float ShadowStrength;             // 阴影强度
    float Bias;                       // 深度偏移
    float NormalBias;                 // 法线偏移（点光源=0）
    float2 Pad1;                      // 对齐到 16 字节边界
    float3 LightPosition;             // 点光源位置（方向光=0）
    float Range;                      // 点光源衰减范围（方向光=0）
    row_major float4x4 LightViewProj; // 方向光 VP 矩阵（点光源填 0）
};

// ============================================================================
// 资源绑定
// ============================================================================

StructuredBuffer<ShadowParams> gShadowParams : register(t11, space1);
Texture2DArray gShadowMaps[] : register(t14, space1);

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
    ShadowParams shadow = gShadowParams[shadowIdx];

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
            shadowFactor += gShadowMaps[NonUniformResourceIndex(shadow.ShadowMapIndex)].SampleCmpLevelZero(
                gShadowSampler, float3(offsetUV, 0), compareDepth);
        }
    }

    shadowFactor /= 9.0f;
    return lerp(1.0f, shadowFactor, shadow.ShadowStrength);
}

// 点光源阴影采样（6 独立面，gShadowMaps[ShadowMapIndex + face]）
// nearZ 必须与 CPU 端 LightManager::ComputePointShadowMatrices 一致
static const float kPointShadowNearZ = 1.0f;

float SamplePointShadow(uint index, float3 worldPos)
{
    ShadowParams shadow = gShadowParams[index];
    float3 L = worldPos - shadow.LightPosition;
    float distance = length(L);

    if (distance >= shadow.Range)
        return 1.0f;

    float3 dir = L / distance;
    float3 absDir = abs(dir);

    // 确定 cube 面 + 计算 UV + 获取 view_z（沿面视线方向的距离）
    uint face;
    float viewZ;
    float2 uv;

    [branch] if (absDir.x >= absDir.y && absDir.x >= absDir.z)
    {
        viewZ = abs(L.x);
        if (dir.x >= 0)
        {
            face = 0;
            uv = float2(-dir.z, dir.y) / absDir.x;
        } // +X
        else
        {
            face = 1;
            uv = float2(dir.z, dir.y) / absDir.x;
        } // -X
    }
    else if (absDir.y >= absDir.z)
    {
        viewZ = abs(L.y);
        if (dir.y >= 0)
        {
            face = 2;
            uv = float2(dir.x, -dir.z) / absDir.y;
        } // +Y
        else
        {
            face = 3;
            uv = float2(dir.x, dir.z) / absDir.y;
        } // -Y
    }
    else
    {
        viewZ = abs(L.z);
        if (dir.z >= 0)
        {
            face = 4;
            uv = float2(dir.x, dir.y) / absDir.z;
        } // +Z
        else
        {
            face = 5;
            uv = float2(-dir.x, dir.y) / absDir.z;
        } // -Z
    }

    // [-1, 1] → [0, 1], flip Y for DX
    uv = uv * 0.5f + 0.5f;
    uv.y = 1.0f - uv.y;

    // 计算与阴影贴图存储深度一致的比较值（LH 透视投影公式）
    float farZ = shadow.Range;
    float Q = farZ / (farZ - kPointShadowNearZ);
    float compareDepth = Q * (1.0f - kPointShadowNearZ / viewZ) - shadow.Bias;

    uint shadowIdx = shadow.ShadowMapIndex;
    float sampled = gShadowMaps[NonUniformResourceIndex(shadowIdx)].SampleCmpLevelZero(
        gShadowSampler, float3(uv, face), compareDepth);

    return lerp(1.0f, sampled, shadow.ShadowStrength);
}

#endif // SHADOW_SAMPLING_HLSL