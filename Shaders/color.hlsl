// =================================================================================================
// Includes
// =================================================================================================
#include "LightingUtil.hlsl"

// =================================================================================================
// cbPerObject: 每物体常量缓冲
// =================================================================================================
cbuffer cbPerObject : register(b0)
{
    row_major float4x4 gWorld;
    row_major float4x4 gWorldInvTrans;
};

// =================================================================================================
// cbPass: 每帧常量缓冲
// =================================================================================================
cbuffer cbPass : register(b1)
{
    row_major float4x4 gView;
    row_major float4x4 gProj;
    row_major float4x4 gViewProj;
    row_major float4x4 gInvView;
    row_major float4x4 gInvProj;
    row_major float4x4 gInvViewProj;
    float3 gCameraPos;
    float gTotalTime;
    float gDeltaTime;
    float gNearPlane;
    float gFarPlane;
    float gAspectRatio;
    uint gFrameCount;
    float4 gAmbientLight;
    float gPad[3];
};

// =================================================================================================
// cbMaterial: 材质常量缓冲
// =================================================================================================
cbuffer cbMaterial : register(b2)
{
    float4 gBaseColor;
    float gMetallic;
    float gRoughness;
    float gAmbient;
    float gAlpha;
    float4 gEmissive;
    float gAlphaCutoff;
    uint gBaseColorTextureIndex;
    uint gNormalTextureIndex;
    uint gMetallicRoughnessTextureIndex;
    uint gEmissiveTextureIndex;
    uint gOcclusionTextureIndex;
    float gPad2[3];
};

// =================================================================================================
// cbLights: 光源常量缓冲
// =================================================================================================
cbuffer cbLights : register(b3)
{
    Light gLights[256];
    uint gNumDirLights;
    uint gNumPointLights;
    uint gNumSpotLights;
    uint gLightsPad[5];
};

// =================================================================================================
// 顶点输入/输出
// =================================================================================================
struct VertexIn
{
    float3 PosL : POSITION;
    float4 ColorL : COLOR;
    float3 NormalL : NORMAL;
};

struct VertexOut
{
    float4 PosH : SV_POSITION;
    float3 WorldPos : POSITION;
    float3 WorldNormal : NORMAL;
    float4 Color : COLOR;
};

// =================================================================================================
// 顶点着色器
// =================================================================================================
VertexOut VS(VertexIn vin)
{
    VertexOut vout;

    float4 worldPos = mul(float4(vin.PosL, 1.0f), gWorld);
    vout.WorldPos = worldPos.xyz;
    vout.PosH = mul(worldPos, gViewProj);

    vout.WorldNormal = normalize(mul(vin.NormalL, (float3x3)gWorldInvTrans));
    vout.Color = vin.ColorL;

    return vout;
}

// =================================================================================================
// 像素着色器 - PBR 版本
// =================================================================================================
float4 PS(VertexOut pin) : SV_Target
{
    float3 N = normalize(pin.WorldNormal);
    float3 V = normalize(gCameraPos - pin.WorldPos);

    // 构建 PBR 材质
    Material mat;
    mat.BaseColor = gBaseColor;
    mat.Metallic = gMetallic;
    mat.Roughness = gRoughness;
    mat.Ambient = gAmbient; // 环境光遮蔽强度
    mat.Emissive = gEmissive;

    // 环境光 (考虑 Ambient)
    float3 ambient = gAmbientLight.xyz * gAmbientLight.w * mat.BaseColor.xyz * mat.Ambient;

    // 直接光照
    float3 directLight = float3(0.0f, 0.0f, 0.0f);

    // 方向光
    for (uint i = 0; i < gNumDirLights; ++i)
    {
        directLight += ComputeDirectionalLight(gLights[i], mat, N, V);
    }

    // 点光源
    for (uint j = gNumDirLights; j < gNumDirLights + gNumPointLights; ++j)
    {
        directLight += ComputePointLight(gLights[j], mat, pin.WorldPos, N, V);
    }

    // 聚光灯
    for (uint k = gNumDirLights + gNumPointLights;
         k < gNumDirLights + gNumPointLights + gNumSpotLights;
         ++k)
    {
        directLight += ComputeSpotLight(gLights[k], mat, pin.WorldPos, N, V);
    }

    // 自发光
    float3 emissive = mat.Emissive.xyz * mat.Emissive.w;

    // 合并光照并应用顶点颜色
    float3 litColor = (ambient + directLight + emissive) * pin.Color.rgb;

    return float4(litColor, gBaseColor.a * pin.Color.a);
}