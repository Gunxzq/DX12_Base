

struct PassConstants
{
    row_major float4x4 View;
    row_major float4x4 Proj;
    row_major float4x4 ViewProj;
    row_major float4x4 InvView;
    row_major float4x4 InvProj;
    row_major float4x4 InvViewProj;
    row_major float4x4 PrevViewProj;
    float3 CameraPos;
    float TotalTime;
    float DeltaTime;
    float NearPlane;
    float FarPlane;
    float AspectRatio;
    uint FrameCount;
    float3 Pad;
};
cbuffer cbPass : register(b1) { PassConstants gPass; };

struct InstanceData
{
    row_major float4x4 World;
    row_major float4x4 WorldInvTranspose;
    uint MaterialIndex;
    uint ReceiveShadow;
    uint ProbeIndex;
    float pad;
};
StructuredBuffer<InstanceData> gInstanceData : register(t12, space1);
StructuredBuffer<float4x4> gBoneTransforms : register(t13, space1);

struct VSInput
{
    float3 PosL : POSITION;
    float3 TangentU : TANGENT;
    float3 NormalL : NORMAL;
    float2 TexC : TEXCOORD;
    float4 BoneWeights : BLENDWEIGHTS;
    uint4 BoneIndices : BLENDINDICES;
};

struct VSOutput
{
    float4 PosH : SV_POSITION;
    float3 PosW : POSITION;
    float3 NormalW : NORMAL;
    float3 TangentW : TANGENT;
    float2 TexC : TEXCOORD;
    uint MatIdx : MATERIAL_INDEX;
    uint RtShad : RECEIVE_SHADOW;
};

// ========================================================================
// VS — 蒙皮顶点变换
// ========================================================================
VSOutput VS(VSInput vin, uint instanceId : SV_InstanceID)
{
    VSOutput vout;
    InstanceData inst = gInstanceData[instanceId];

    // 蒙皮：加权骨骼变换（行向量乘法，配合 C++ offset * toRoot 公式）
    float3 posL = 0, normalL = 0, tangentL = 0;
    [unroll] for (int i = 0; i < 4; i++)
    {
        float w = vin.BoneWeights[i];
        if (w > 0)
        {
            float4x4 bm = gBoneTransforms[vin.BoneIndices[i]];
            posL += w * mul(float4(vin.PosL, 1.0f), bm).xyz;
            normalL += w * mul(vin.NormalL, (float3x3)bm);
            tangentL += w * mul(vin.TangentU, (float3x3)bm);
        }
    }

    float4 posW = mul(float4(posL, 1.0f), inst.World);
    vout.PosH = mul(posW, gPass.ViewProj);
    vout.PosW = posW.xyz;
    vout.NormalW = mul(normalL, (float3x3)inst.WorldInvTranspose);
    vout.TangentW = mul(tangentL, (float3x3)inst.World);
    vout.TexC = vin.TexC;
    vout.MatIdx = inst.MaterialIndex;
    vout.RtShad = inst.ReceiveShadow;
    return vout;
}

// ========================================================================
// 材质数据与纹理资源（PS_GBuffer 共用）
// ========================================================================
struct MaterialData
{
    float4 BaseColor;
    float Metallic;
    float Roughness;
    float Ambient;
    float Alpha;
    float4 Emissive;
    float AlphaCutoff;
    float NormalStrength;
    uint BaseColorTextureIndex;
    uint NormalTextureIndex;
    uint MetallicRoughnessTextureIndex;
    uint EmissiveTextureIndex;
    uint OcclusionTextureIndex;
    uint HeightTextureIndex;
    uint OpacityTextureIndex;
    uint MaskTextureIndex;
    uint SubsurfaceTextureIndex;
    uint ClearCoatTextureIndex;
};
StructuredBuffer<MaterialData> gMaterialData : register(t0, space1);

// 共用纹理堆（与 color.hlsl 共用同一描述符表，register 映射见 SkinnedRenderer 根签名 slot 3）
Texture2D gTextureMaps[] : register(t0, space2);

SamplerState gsamPointWrap : register(s0);
SamplerState gsamLinearWrap : register(s1);
SamplerState gsamAnisotropicWrap : register(s2);

// 法线贴图 TBN 变换
float3 NormalSampleToWorldSpace(float3 normalMapSample, float3 unitNormalW, float3 tangentW)
{
    float3 normalT = 2.0f * normalMapSample - 1.0f;
    float lenSq = dot(normalT.xy, normalT.xy);
    normalT.z = sqrt(max(0.0f, 1.0f - lenSq));
    float3 N = unitNormalW;
    float3 T = normalize(tangentW - dot(tangentW, N) * N);
    float3 B = cross(N, T);
    return mul(normalT, float3x3(T, B, N));
}

// ========================================================================
// PS_GBuffer — G-buffer MRT 输出（复用蒙皮 VS）
// ========================================================================
struct GBufferOutput
{
    float4 Albedo : SV_Target0;
    float4 Normal : SV_Target1;
    float4 Material : SV_Target2;
    float4 WorldPos : SV_Target3;
    float4 Emissive : SV_Target4; // 自发光（不参与光照，直接叠加）
};

GBufferOutput PS_GBuffer(VSOutput pin)
{
    GBufferOutput output = (GBufferOutput)0.0f;

    MaterialData mat = gMaterialData[pin.MatIdx];

    // BaseColor
    float3 baseColor = mat.BaseColor.rgb;
    if (mat.BaseColorTextureIndex != 0xFFFFFFFF)
        baseColor *= gTextureMaps[mat.BaseColorTextureIndex].Sample(gsamLinearWrap, pin.TexC).rgb;
    float3 albedo = baseColor;
    float metallic = mat.Metallic;
    float roughness = mat.Roughness;
    float ao = mat.Ambient;

    // MetallicRoughness 贴图
    if (mat.MetallicRoughnessTextureIndex != 0xFFFFFFFF)
    {
        float2 mr = gTextureMaps[mat.MetallicRoughnessTextureIndex].Sample(gsamAnisotropicWrap, pin.TexC).rg;
        metallic = mr.r;
        roughness = mr.g;
    }
    // Occlusion 贴图
    if (mat.OcclusionTextureIndex != 0xFFFFFFFF)
        ao = gTextureMaps[mat.OcclusionTextureIndex].Sample(gsamAnisotropicWrap, pin.TexC).r;

    // 法线 TBN 变换
    float3 N = normalize(pin.NormalW);
    if (mat.NormalTextureIndex != 0xFFFFFFFF)
    {
        float4 normalSample = gTextureMaps[mat.NormalTextureIndex].Sample(gsamAnisotropicWrap, pin.TexC);
        float3 nm = lerp(float3(0.5f, 0.5f, 1.0f), normalSample.rgb, mat.NormalStrength);
        float3 tangentW = normalize(pin.TangentW);
        N = NormalSampleToWorldSpace(nm, N, tangentW);
    }

    output.Albedo = float4(albedo, 1.0f);
    output.Normal = float4(N * 0.5f + 0.5f, 1.0f);
    output.Material = float4(metallic, roughness, ao, 0.0f); // probeIndex 暂为 0
    output.WorldPos = float4(pin.PosW, 1.0f);
    output.Emissive = float4(mat.Emissive.rgb, 1.0f); // 自发光（HDR），不参与光照
    return output;
}
