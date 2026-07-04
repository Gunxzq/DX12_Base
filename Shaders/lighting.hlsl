// lighting.hlsl — 延迟渲染光照 Pass
#include "LightingUtil.hlsl"

// 阴影比较采样器（ShadowSampling.hlsl 依赖此声明）
SamplerComparisonState gShadowSampler : register(s11);

#include "ShadowSampling.hlsl"

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

Texture2D gAlbedoRT : register(t20);
Texture2D gNormalRT : register(t21);
Texture2D gMaterialRT : register(t22);
Texture2D gWorldPosRT : register(t23);
Texture2D gSsaoMap : register(t16);

SamplerState gSamplerPointClamp : register(s3);

// 环境反射贴图（从天空盒产生）
TextureCube gEnvMap : register(t10);
SamplerState gEnvSampler : register(s10);

// 动态反射探针 Cubemap Array
TextureCubeArray gReflectionCubemapArray : register(t15);

// 环境反射（延迟渲染专用，支持反射探针 Cubemap Array 与天空盒回退）
float3 ComputeEnvironmentReflectionDeferred(float3 reflectDir, float3 albedo, float metallic, float roughness, float3 N, float3 V, uint probeIndex)
{
    float3 reflection = 0;
    if (probeIndex > 0)
        reflection = gReflectionCubemapArray.Sample(gEnvSampler, float4(reflectDir, probeIndex - 1)).rgb;
    else
        reflection = gEnvMap.Sample(gEnvSampler, reflectDir).rgb;
    float3 F0 = lerp(0.04f, albedo, metallic);
    float NdotV = max(dot(N, V), 0.0f);
    float3 fresnel = F0 + (1.0f - F0) * pow(1.0f - NdotV, 5.0f);
    return reflection * fresnel * (1.0f - roughness * 0.5f);
}

struct QuadOut
{
    float4 PosH : SV_POSITION;
    float2 UV : TEXCOORD;
};

QuadOut VS(uint vertexID : SV_VertexID)
{
    QuadOut v;
    float2 uv = float2(vertexID & 1, (vertexID >> 1) & 1);
    v.UV = float2(uv.x, 1.0f - uv.y);
    v.PosH = float4(uv * 2.0f - 1.0f, 0.0f, 1.0f);
    return v;
}

float4 PS(QuadOut pin) : SV_Target
{
    float3 albedo = gAlbedoRT.Sample(gSamplerPointClamp, pin.UV).rgb;
    float3 N = normalize(gNormalRT.Sample(gSamplerPointClamp, pin.UV).xyz * 2.0f - 1.0f);
    float4 mat = gMaterialRT.Sample(gSamplerPointClamp, pin.UV);
    float metallic = mat.r, roughness = mat.g, ao = mat.b;
    float3 worldPos = gWorldPosRT.Sample(gSamplerPointClamp, pin.UV).xyz;
    float3 V = normalize(gCameraPos - worldPos);
    float ssao = gSsaoMap.SampleLevel(gSamplerPointClamp, pin.UV, 0.0f).r;
    float3 ambient = gAmbientLight.xyz * gAmbientLight.w * albedo * ao * ssao;
    float3 direct = 0;
    for (uint i = 0; i < gNumDirLights; ++i)
    {
        float3 lightContrib = ComputeDirectionalLightDeferred(gLights[i], albedo, metallic, roughness, N, V);
        // 方向光阴影（光源有 ShadowMapIndex 且投射阴影时采样）
        if (gLights[i].ShadowMapIndex < 255 && gLights[i].CastShadow > 0.5f)
        {
            lightContrib *= SampleDirShadow(gLights[i].ShadowMapIndex, worldPos, N, gLights[i].Direction.xyz);
        }
        direct += lightContrib;
    }

    // 点光源
    for (uint j = 0; j < gNumPointLights; ++j)
    {
        uint idx = gNumDirLights + j;
        Light ptLight = gLights[idx];
        // 点光源使用 Position 计算 L 向量，不能用 Direction（默认值为 0）
        float3 L = ptLight.Position.xyz - worldPos;
        float dist = length(L);
        L /= dist;
        float atten = saturate((ptLight.FalloffEnd - dist) / (ptLight.FalloffEnd - ptLight.FalloffStart));
        if (atten <= 0.0f)
            continue;

        float NdotL = max(dot(N, L), 0.0f);
        float3 H = normalize(V + L);
        float NdotH = max(dot(N, H), 0.0f);
        float NdotV = max(dot(N, V), 0.0f);
        float NdotH2 = NdotH * NdotH;
        float3 radiance = ptLight.Strength * atten;
        float3 F0 = lerp(0.04f, albedo, metallic);
        float3 F = F0 + (1.0f - F0) * pow(1.0f - NdotH, 5.0f);
        float a = roughness * roughness;
        float a2 = a * a;
        float D = a2 / (3.14159f * (NdotH2 * (a2 - 1.0f) + 1.0f) * (NdotH2 * (a2 - 1.0f) + 1.0f));
        float k = (roughness + 1.0f) * (roughness + 1.0f) / 8.0f;
        float G = (NdotV / (NdotV * (1.0f - k) + k)) * (NdotL / (NdotL * (1.0f - k) + k));
        float3 specular = D * G * F / (4.0f * max(NdotV, 0.01f) * max(NdotL, 0.01f) + 0.0001f);
        float3 kD = (1.0f - F) * (1.0f - metallic);
        float3 diffuse = kD * albedo / 3.14159f;
        float3 lightContrib = (diffuse + specular) * radiance * NdotL;
        // 点光源阴影（ShadowMapIndex < 255 且投射阴影）
        if (ptLight.ShadowMapIndex < 255 && ptLight.CastShadow > 0.5f)
        {
            lightContrib *= SamplePointShadow(ptLight.ShadowMapIndex, worldPos);
        }
        direct += lightContrib;
    }

    // 环境反射（由 G-buffer Material.a 编码的 probeIndex 控制）
    uint probeIndex = (uint)(mat.a * 255.0f);
    float3 reflection = ComputeEnvironmentReflectionDeferred(reflect(-V, N), albedo, metallic, roughness, N, V, probeIndex);

    return float4(ambient + direct + reflection, 1.0f);
}
