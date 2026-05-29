#include "Common_PBR.hlsl"

Texture2D gTexture : register(t0);

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
    float4 worldPos = mul(float4(vin.PosL, 1.0f), gWorld);
    vout.WorldPos = worldPos.xyz;
    vout.PosH = mul(worldPos, gViewProj);
    vout.WorldNormal = normalize(mul(vin.NormalL, (float3x3)gWorldInvTrans));
    vout.WorldTangent = normalize(mul(vin.TangentL, (float3x3)gWorld));
    vout.TexCoord = clamp(vin.TexCoord, 0.0f, 0.999f);
    return vout;
}

float4 PS(VertexOut pin) : SV_Target
{
    MaterialData matData = gMaterialData[gMaterialIndex];

    float4 texColor = gTexture.Sample(gSampler, pin.TexCoord);
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

    // 环境光
    float3 ambient = gAmbientLight.xyz * gAmbientLight.w * albedo * ao;

    // 直接光照
    float3 directLight = 0;
    for (uint i = 0; i < gNumDirLights; ++i)
        directLight += ComputeDirectionalLight(gLights[i], mat, N, V);
    for (uint j = gNumDirLights; j < gNumDirLights + gNumPointLights; ++j)
        directLight += ComputePointLight(gLights[j], mat, pin.WorldPos, N, V);
    for (uint k = gNumDirLights + gNumPointLights; k < gNumDirLights + gNumPointLights + gNumSpotLights; ++k)
        directLight += ComputeSpotLight(gLights[k], mat, pin.WorldPos, N, V);

    // 环境反射
    float3 R = reflect(-V, N);
    float3 reflection = ComputeEnvironmentReflection(R, albedo, metallic, roughness, N, V);

    float3 litColor = ambient + directLight + reflection + emissive;

    return float4(litColor, mat.BaseColor.a);
}