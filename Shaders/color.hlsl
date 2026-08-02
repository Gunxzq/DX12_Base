//==============================================================================
// color.hlsl - PBR 实体渲染着色器（带阴影采样 + 动态反射探针）
// 统一实例化模式：所有物体通过 StructuredBuffer<InstanceData> 传入
//==============================================================================

#include "Common_PBR.hlsl"
#include "ShadowSampling.hlsl"

// =========================================================================
// 动态反射探针 Cubemap Array（仅在此着色器中使用）
// TextureCubeArray，由 GetProbeCubemapArraySRV 提供
// probeIndex 作为 array index，最大 64 个探针
// =========================================================================
TextureCubeArray gReflectionCubemapArray : register(t15);

// =========================================================================
// SSAO Map — 屏幕空间环境光遮蔽（由 SsaoRenderer Compute+Blur 生成）
// 若未绑定则为全白（ssao=1.0）
// =========================================================================
Texture2D gSsaoMap : register(t16);
SamplerState gsamPointClamp : register(s0);

// =========================================================================
// 实例数据（统一实例化模式，单物体 instanceCount=1）
// =========================================================================
struct InstanceData
{
    row_major float4x4 World;
    row_major float4x4 WorldInvTranspose;
    uint MaterialIndex;
    uint ReceiveShadow;
    uint ProbeIndex; // 反射探针索引 (0xFFFFFFFF = 无反射)
    float pad;
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
    float4 SsaoPosH : POSITION1; // 投影后的 SSAO 纹理坐标
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

    // 计算 SSAO 投影纹理坐标（mul(posW, gViewProj) + NDC→UV 变换由 PS 处理）
    vout.SsaoPosH = mul(float4(vout.WorldPos, 1.0f), gViewProj);

    return vout;
}

// 反射探针环境反射（TextureCubeArray，probeIndex 选探针）
float3 ComputeProbeReflection(float3 reflectDir, float3 albedo, float metallic, float roughness, float3 N, float3 V, uint probeIndex)
{
    if (probeIndex == 0xFFFFFFFF)
        return float3(0.0f, 0.0f, 0.0f);

    float3 reflection = gReflectionCubemapArray.Sample(gSamplerLinearClamp, float4(reflectDir, probeIndex)).rgb;
    float3 F0 = lerp(0.04f, albedo, metallic);
    float NdotV = max(dot(N, V), 0.0f);
    float3 fresnel = FresnelSchlick(NdotV, F0);
    float strength = 1.0f - roughness * 0.5f;
    return reflection * fresnel * strength;
}

// 法线贴图 TBN 变换
float3 NormalSampleToWorldSpace(float3 normalMapSample, float3 unitNormalW, float3 tangentW)
{
    float3 normalT = 2.0f * normalMapSample - 1.0f;
    // BC5 法线贴图只存 R/G，重建 Z
    float lenSq = dot(normalT.xy, normalT.xy);
    normalT.z = sqrt(max(0.0f, 1.0f - lenSq));
    float3 N = unitNormalW;
    float3 T = normalize(tangentW - dot(tangentW, N) * N);
    float3 B = cross(N, T);
    return mul(normalT, float3x3(T, B, N));
}

// ========================================================================
// PS_GBuffer — G-buffer MRT 输出（复用 VS）
// SV_Target0: Albedo       (R8G8B8A8_UNORM)
// SV_Target1: Normal       (R16G16B16A16_FLOAT) — 世界空间法线
// SV_Target2: Material     (R8G8B8A8_UNORM) — R=Metallic, G=Roughness, B=AO
// SV_Target3: WorldPos     (R16G16B16A16_FLOAT)
// ========================================================================
struct GBufferOutput
{
    float4 Albedo : SV_Target0;
    float4 Normal : SV_Target1;
    float4 Material : SV_Target2;
    float4 WorldPos : SV_Target3;
    float4 Emissive : SV_Target4; // 自发光（不参与光照，直接叠加）
};

GBufferOutput PS_GBuffer(VertexOut pin)
{
    GBufferOutput output = (GBufferOutput)0.0f;

    uint matIndex = gInstanceData[pin.InstanceIndex].MaterialIndex;
    MaterialData matData = gMaterialData[matIndex];

    float4 texColor = 1.0f;
    [branch] if (matData.BaseColorTexIndex != 0xFFFFFFFF)
    {
        texColor = gTextureMaps[matData.BaseColorTexIndex].Sample(gSamplerAnisotropicWrap, pin.TexCoord);
    }
    float3 albedo = matData.BaseColor.rgb * texColor.rgb;
    float metallic = matData.Metallic;
    float roughness = matData.Roughness;
    float ao = matData.Ambient;

    [branch] if (matData.MetallicRoughnessTexIndex != 0xFFFFFFFF)
    {
        float2 mr = gTextureMaps[matData.MetallicRoughnessTexIndex].Sample(gSamplerAnisotropicWrap, pin.TexCoord).rg;
        metallic = mr.r;
        roughness = mr.g;
    }
    [branch] if (matData.OcclusionTexIndex != 0xFFFFFFFF)
    {
        ao = gTextureMaps[matData.OcclusionTexIndex].Sample(gSamplerAnisotropicWrap, pin.TexCoord).r;
    }

    // 法线贴图 TBN 变换
    float3 N = normalize(pin.WorldNormal);
    [branch] if (matData.NormalTexIndex != 0xFFFFFFFF)
    {
        float4 normalSample = gTextureMaps[matData.NormalTexIndex].Sample(gSamplerAnisotropicWrap, pin.TexCoord);
        normalSample.rgb = lerp(float3(0.5f, 0.5f, 1.0f), normalSample.rgb, matData.NormalStrength);
        N = NormalSampleToWorldSpace(normalSample.rgb, N, normalize(pin.WorldTangent));
    }

    output.Albedo = float4(albedo, 1.0f);
    output.Normal = float4(N * 0.5f + 0.5f, 1.0f);
    output.Material = float4(metallic, roughness, ao,
                             (gInstanceData[pin.InstanceIndex].ProbeIndex != 0xFFFFFFFF)
                                 ? (gInstanceData[pin.InstanceIndex].ProbeIndex + 1) / 255.0f
                                 : 0.0f);
    output.WorldPos = float4(pin.WorldPos, 1.0f);
    output.Emissive = float4(matData.Emissive.rgb, 1.0f); // 自发光（HDR），不参与光照

    return output;
}
