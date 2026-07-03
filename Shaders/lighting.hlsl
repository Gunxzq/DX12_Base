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

// Schlick 菲涅尔近似
float3 FresnelSchlick2(float cosTheta, float3 F0)
{
    return F0 + (1.0f - F0) * pow(1.0f - cosTheta, 5.0f);
}

// 环境反射计算
float3 ComputeEnvironmentReflection2(float3 reflectDir, float3 albedo, float metallic, float roughness, float3 N, float3 V)
{
    float3 reflection = gEnvMap.Sample(gEnvSampler, reflectDir).rgb;
    float3 F0 = lerp(0.04f, albedo, metallic);
    float NdotV = max(dot(N, V), 0.0f);
    float3 fresnel = FresnelSchlick2(NdotV, F0);
    float strength = 1.0f - roughness * 0.5f;
    return reflection * fresnel * strength;
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

float3 ComputeDirectionalLight2(Light light, float3 albedo, float metallic, float roughness, float3 N, float3 V)
{
    float3 L = -light.Direction;
    float NdotL = max(dot(N, L), 0.0f);
    float3 H = normalize(V + L);
    float NdotH = max(dot(N, H), 0.0f);
    float NdotV = max(dot(N, V), 0.0f);
    float3 radiance = light.Strength;
    float3 F0 = lerp(0.04f, albedo, metallic);
    float3 F = F0 + (1.0f - F0) * pow(1.0f - NdotH, 5.0f);
    float a = roughness * roughness;
    float a2 = a * a;
    float NdotH2 = NdotH * NdotH;
    float D = a2 / (3.14159f * (NdotH2 * (a2 - 1.0f) + 1.0f) * (NdotH2 * (a2 - 1.0f) + 1.0f));
    float k = (roughness + 1.0f) * (roughness + 1.0f) / 8.0f;
    float G = (NdotV / (NdotV * (1.0f - k) + k)) * (NdotL / (NdotL * (1.0f - k) + k));
    float3 specular = D * G * F / (4.0f * max(NdotV, 0.01f) * max(NdotL, 0.01f) + 0.0001f);
    float3 kD = (1.0f - F) * (1.0f - metallic);
    float3 diffuse = kD * albedo / 3.14159f;
    return (diffuse + specular) * radiance * NdotL;
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
        float3 lightContrib = ComputeDirectionalLight2(gLights[i], albedo, metallic, roughness, N, V);
        // 方向光阴影（光源有 ShadowMapIndex 且有效时采样）
        if (gLights[i].ShadowMapIndex < 255)
        {
            lightContrib *= SampleDirShadow(gLights[i].ShadowMapIndex, worldPos, N, gLights[i].Direction.xyz);
        }
        direct += lightContrib;
    }

    // 环境反射（由 G-buffer Material.a 编码的 probeIndex 控制）
    float3 reflectDir = reflect(-V, N);
    float3 reflection = 0;
    uint probeIndex = (uint)(mat.a * 255.0f);
    if (probeIndex > 0)
    {
        // 有专用探针 → 采样 Cubemap Array
        reflection = gReflectionCubemapArray.Sample(gEnvSampler, float4(reflectDir, probeIndex - 1)).rgb;
    }
    else
    {
        // 无探针 → 回退天空盒环境贴图
        reflection = gEnvMap.Sample(gEnvSampler, reflectDir).rgb;
    }
    // Fresnel 衰减 + 粗糙度衰减
    float3 F0 = lerp(0.04f, albedo, metallic);
    float NdotV_ref = max(dot(N, V), 0.0f);
    float3 fresnel = F0 + (1.0f - F0) * pow(1.0f - NdotV_ref, 5.0f);
    reflection *= fresnel * (1.0f - roughness * 0.5f);

    return float4(ambient + direct + reflection, 1.0f);
}
