//==============================================================================
// color.hlsl - PBR 实体渲染着色器（带阴影采样）
// 统一实例化模式：所有物体通过 StructuredBuffer<InstanceData> 传入
//==============================================================================
#define DISABLE_ENV_REFLECTION

#include "Common_PBR.hlsl"
#include "ShadowSampling.hlsl"

Texture2D gTexture : register(t0);

// =========================================================================
// 实例数据（统一实例化模式，单物体 instanceCount=1）
// =========================================================================
struct InstanceData
{
    row_major float4x4 World;
    row_major float4x4 WorldInvTranspose;
    uint MaterialIndex;
    uint ReceiveShadow;
};

StructuredBuffer<InstanceData> gInstanceData : register(t12, space1);

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
    nointerpolation uint InstanceIndex : INSTANCE_INDEX;
};

VertexOut VS(VertexIn vin, uint instanceID : SV_InstanceID)
{
    VertexOut vout;

    InstanceData inst = gInstanceData[instanceID];
    float4x4 world = inst.World;
    float4x4 worldInvTrans = inst.WorldInvTranspose;
    vout.InstanceIndex = instanceID;

    float4 worldPos = mul(float4(vin.PosL, 1.0f), world);
    vout.WorldPos = worldPos.xyz;
    vout.PosH = mul(worldPos, gViewProj);
    vout.WorldNormal = normalize(mul(vin.NormalL, (float3x3)worldInvTrans));
    vout.WorldTangent = normalize(mul(vin.TangentL, (float3x3)world));
    vout.TexCoord = clamp(vin.TexCoord, 0.0f, 0.999f);
    return vout;
}

float4 PS(VertexOut pin) : SV_Target
{
    uint matIndex = gInstanceData[pin.InstanceIndex].MaterialIndex;
    uint receiveShadow = gInstanceData[pin.InstanceIndex].ReceiveShadow;

    MaterialData matData = gMaterialData[matIndex];

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

    // 直接光照（带阴影）
    float3 directLight = 0;
    for (uint i = 0; i < gNumDirLights; ++i)
    {
        float3 lightContrib = ComputeDirectionalLight(gLights[i], mat, N, V);

        // 方向光阴影采样
        if (gLights[i].ShadowMapIndex >= 0 && receiveShadow)
        {
            float shadow = SampleDirShadow((uint)gLights[i].ShadowMapIndex, pin.WorldPos, N, gLights[i].Direction.xyz);
            lightContrib *= shadow;
        }

        directLight += lightContrib;
    }
    for (uint j = gNumDirLights; j < gNumDirLights + gNumPointLights; ++j)
        directLight += ComputePointLight(gLights[j], mat, pin.WorldPos, N, V);
    for (uint k = gNumDirLights + gNumPointLights; k < gNumDirLights + gNumPointLights + gNumSpotLights; ++k)
        directLight += ComputeSpotLight(gLights[k], mat, pin.WorldPos, N, V);

    // 环境反射
    float3 reflection = 0.0f;

    float3 litColor = ambient + directLight + reflection + emissive;

    return float4(litColor, mat.BaseColor.a);
}
