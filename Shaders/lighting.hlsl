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
Texture2D gEmissiveRT : register(t24);
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

    uint totalLights = gNumDirLights + gNumPointLights + gNumSpotLights;
    for (uint i = 0; i < totalLights; ++i)
    {
        Light light = gLights[i];
        float3 lightContrib = 0;

        [branch] if (light.Type == 0) // Directional
        {
            lightContrib = ComputeDirectionalLightDeferred(light, albedo, metallic, roughness, N, V);
            if (light.CastShadow > 0.5f)
                lightContrib *= SampleShadow(light, worldPos, N);
        }
        else if (light.Type == 1) // Point
        {
            lightContrib = ComputePointLightDeferred(light, albedo, metallic, roughness, N, V, worldPos);
            if (light.CastShadow > 0.5f)
                lightContrib *= SampleShadow(light, worldPos, N);
        }
        else // Spot
        {
            lightContrib = ComputeSpotLightDeferred(light, albedo, metallic, roughness, N, V, worldPos);
            if (light.CastShadow > 0.5f)
                lightContrib *= SampleShadow(light, worldPos, N);
        }

        direct += lightContrib;
    }

    // 环境反射（由 G-buffer Material.a 编码的 probeIndex 控制）
    uint probeIndex = (uint)(mat.a * 255.0f);
    float3 reflection = ComputeEnvironmentReflectionDeferred(reflect(-V, N), albedo, metallic, roughness, N, V, probeIndex);

    // 自发光（G-buffer Emissive 通道，HDR，不参与光照直接叠加）
    float3 emissive = gEmissiveRT.Sample(gSamplerPointClamp, pin.UV).rgb;

    return float4(ambient + direct + reflection + emissive, 1.0f);
}
