

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
// PS — 纹理化漫反射光照（支持 BaseColor + Normal 贴图）
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
    // BC5 法线贴图只存 R/G，重建 Z
    float lenSq = dot(normalT.xy, normalT.xy);
    normalT.z = sqrt(max(0.0f, 1.0f - lenSq));
    float3 N = unitNormalW;
    float3 T = normalize(tangentW - dot(tangentW, N) * N);
    float3 B = cross(N, T);
    return mul(normalT, float3x3(T, B, N));
}

float4 PS(VSOutput pin) : SV_Target
{
    MaterialData mat = gMaterialData[pin.MatIdx];

    // BaseColor = 材质固有色 * 漫反射贴图
    float3 baseColor = mat.BaseColor.rgb;
    if (mat.BaseColorTextureIndex != 0xFFFFFFFF)
    {
        baseColor *= gTextureMaps[mat.BaseColorTextureIndex].Sample(gsamLinearWrap, pin.TexC).rgb;
    }

    // 法线贴图
    float3 N = normalize(pin.NormalW);
    if (mat.NormalTextureIndex != 0xFFFFFFFF)
    {
        float4 normalSample = gTextureMaps[mat.NormalTextureIndex].Sample(gsamAnisotropicWrap, pin.TexC);
        // 法线强度调制
        float3 nm = lerp(float3(0.5f, 0.5f, 1.0f), normalSample.rgb, mat.NormalStrength);
        float3 tangentW = normalize(pin.TangentW);
        N = NormalSampleToWorldSpace(nm, N, tangentW);
    }

    // 简单漫反射光照
    float3 L = normalize(float3(0.0, -1.0, -1.0));
    float NdotL = max(0, dot(N, -L));
    float3 ambient = 0.1f * baseColor;
    float3 diffuse = NdotL * baseColor;
    return float4(ambient + diffuse, mat.Alpha);
}
