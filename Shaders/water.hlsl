// water.hlsl - 水面着色器
#include "Common_PBR.hlsl"

Texture2D gTexture : register(t0);

cbuffer cbWater : register(b3)
{
    float gTime;                  // 时间
    float gWaveAmplitude;         // 波幅
    float gWaveFrequency;         // 波频
    float gWaveSpeed;             // 波速
    float gRefractionStrength;    // 折射强度
    float gFresnelPower;          // 菲涅尔功率
    float gFoamIntensity;         // 泡沫强度
    uint gReflectionTextureIndex; // 反射纹理索引
    uint gRefractionTextureIndex; // 折射纹理索引
    uint gDepthTextureIndex;      // 深度纹理索引
    uint gNormalTextureIndex;     // 法线纹理索引
    float gPad1;                  // 填充
}

struct VertexIn
{
    float3 PosL : POSITION;
    float3 NormalL : NORMAL;
    float3 TangentL : TANGENT;
    float2 TexCoord : TEXCOORD;
};

struct VertexOut
{
    float4 PosH : SV_POSITION;
    float3 WorldPos : POSITION;
    float3 WorldNormal : NORMAL;
    float3 WorldTangent : TANGENT;
    float2 TexCoord : TEXCOORD;
};

VertexOut VS(VertexIn vin)
{
    VertexOut vout;

    float time = gTotalTime * gWaveSpeed;

    float y = 0.0f;
    y += sin(vin.PosL.x * gWaveFrequency + time) * cos(vin.PosL.z * gWaveFrequency * 0.8f + time * 0.7f) * gWaveAmplitude;
    y += sin(vin.PosL.x * gWaveFrequency * 2.0f - time * 1.3f) * 0.3f * gWaveAmplitude;
    y += cos(vin.PosL.z * gWaveFrequency * 1.5f + time * 0.9f) * 0.2f * gWaveAmplitude;
    y += sin((vin.PosL.x * 0.5f + vin.PosL.z * 0.5f) * gWaveFrequency * 1.2f + time * 1.1f) * 0.15f * gWaveAmplitude;

    float3 posL = vin.PosL;
    posL.y += y;

    float4 worldPos = mul(float4(posL, 1.0f), gWorld);
    vout.WorldPos = worldPos.xyz;
    vout.PosH = mul(worldPos, gViewProj);

    float3 normalL = vin.NormalL;
    normalL.y += y * 0.5f;
    vout.WorldNormal = normalize(mul(normalL, (float3x3)gWorldInvTrans));
    vout.WorldTangent = normalize(mul(vin.TangentL, (float3x3)gWorld));
    vout.TexCoord = clamp(vin.TexCoord, 0.0f, 0.999f);

    return vout;
}

float4 PS(VertexOut pin) : SV_Target
{
    MaterialData matData = gMaterialData[gMaterialIndex];

    float2 texCoord = pin.TexCoord;
    texCoord.x += gTotalTime * 0.1f;
    texCoord.y += gTotalTime * 0.05f;

    float4 texColor = gTexture.Sample(gSampler, texCoord);
    float3 albedo = matData.BaseColor.rgb * texColor.rgb;
    float metallic = matData.Metallic;
    float roughness = matData.Roughness;
    float ao = matData.Ambient;
    float3 emissive = matData.Emissive.rgb * matData.Emissive.w;

    float3 N = normalize(pin.WorldNormal);
    float3 V = normalize(gCameraPos - pin.WorldPos);

    Material mat;
    mat.BaseColor = float4(albedo, matData.BaseColor.a);
    mat.Metallic = metallic;
    mat.Roughness = roughness;
    mat.Ambient = ao;
    mat.Emissive = float4(emissive, 1.0f);
    mat.Alpha = matData.Alpha;
    mat.AlphaCutoff = matData.AlphaCutoff;

    float3 ambient = gAmbientLight.xyz * gAmbientLight.w * albedo * ao;

    float3 directLight = 0;
    for (uint i = 0; i < gNumDirLights; ++i)
        directLight += ComputeDirectionalLight(gLights[i], mat, N, V);
    for (uint j = gNumDirLights; j < gNumDirLights + gNumPointLights; ++j)
        directLight += ComputePointLight(gLights[j], mat, pin.WorldPos, N, V);
    for (uint k = gNumDirLights + gNumPointLights; k < gNumDirLights + gNumPointLights + gNumSpotLights; ++k)
        directLight += ComputeSpotLight(gLights[k], mat, pin.WorldPos, N, V);
    float3 R = reflect(-V, N);
    float3 reflection = ComputeEnvironmentReflection(R, albedo, metallic, roughness, N, V);

    // 使用 WaterConstants 中的参数
    float waterReflectionStrength = 0.6f;
    float waterDiffuseStrength = 0.3f;
    // 泡沫效果（基于波浪高度 y，只在波峰出现）
    float foam = saturate(pin.WorldPos.y - 9.8f) * gFoamIntensity;
    float3 foamColor = float3(0.9f, 0.9f, 0.8f);

    float3 litColor = ambient + directLight * 0.3f + emissive;

    // 叠加泡沫
    litColor = lerp(litColor, foamColor, foam * 0.3f);

    return float4(litColor, mat.BaseColor.a);
}