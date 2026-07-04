//==============================================================================
// probe_capture.hlsl - 反射探针捕获着色器（GS 实例化）
//
// VS 输出世界空间位置，GS 将每个三角形复制 6 份并设置
// SV_RenderTargetArrayIndex 写入 Cubemap 的对应面。
// 一次 Draw 完成所有 6 面的渲染。
//
// 根签名布局（与 ReflectionProbeRenderer 对齐）:
//   slot 0: b1 cbPass              (CBV)
//   slot 1: b2 cbLights             (CBV)
//   slot 2: t0,space1               StructuredBuffer<MaterialData> (SRV 描述符表)
//   slot 3: t0,space2               Texture2D gTextureMaps[] (纹理堆)
//   slot 4: t12,space1              StructuredBuffer<InstanceData> (SRV)
//   slot 5: b3 cbCapture            (CBV) — 6 面 VP + 探针位置
//==============================================================================

#include "Common_PBR.hlsl"

// =========================================================================
// 实例数据（统一实例化模式）
// =========================================================================
struct InstanceData
{
    row_major float4x4 World;
    row_major float4x4 WorldInvTranspose;
    uint MaterialIndex;
    uint ReceiveShadow;
    uint ProbeIndex; // 捕获时忽略，仅保持结构对齐
    float pad;
};

StructuredBuffer<InstanceData> gInstanceData : register(t12, space1);

// =========================================================================
// 探针捕获常量（b3，CPU 每帧上传）
// =========================================================================
cbuffer cbCapture : register(b3)
{
    row_major float4x4 gFaceViewProj[6];
    float3 gProbePosition;
    float gCapturePad;
};

// =========================================================================
// 顶点输入（与 color.hlsl 一致）
// =========================================================================
struct VertexIn
{
    float3 PosL : POSITION;
    float3 NormalL : NORMAL;
    float3 TangentL : TANGENT;
    float2 TexCoord : TEXCOORD;
};

// =========================================================================
// VS 输出 → GS 输入（世界空间，无投影变换）
// =========================================================================
struct VertexOut
{
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
    vout.WorldNormal = normalize(mul(vin.NormalL, (float3x3)worldInvTrans));
    vout.WorldTangent = normalize(mul(vin.TangentL, (float3x3)world));
    vout.TexCoord = clamp(vin.TexCoord, 0.0f, 0.999f);
    return vout;
}

// =========================================================================
// GS — 将每个三角形复制 6 份，写入不同 Cubemap 面
// =========================================================================
struct GSOutput
{
    float4 PosH : SV_POSITION;
    float3 WorldPos : POSITION;
    float3 WorldNormal : NORMAL;
    float3 WorldTangent : TANGENT;
    float2 TexCoord : TEXCOORD;
    nointerpolation uint InstanceIndex : INSTANCE_INDEX;
    uint RTIndex : SV_RenderTargetArrayIndex;
};

[maxvertexcount(18)] void GS(triangle VertexOut input[3], inout TriangleStream<GSOutput> stream)
{
    [unroll] for (uint face = 0; face < 6; ++face)
    {
        [unroll] for (uint v = 0; v < 3; ++v)
        {
            GSOutput output;
            output.PosH = mul(float4(input[v].WorldPos, 1.0f), gFaceViewProj[face]);
            output.WorldPos = input[v].WorldPos;
            output.WorldNormal = input[v].WorldNormal;
            output.WorldTangent = input[v].WorldTangent;
            output.TexCoord = input[v].TexCoord;
            output.InstanceIndex = input[v].InstanceIndex;
            output.RTIndex = face;
            stream.Append(output);
        }
        stream.RestartStrip();
    }
}

// =========================================================================
// PS — 光照计算（使用探针位置作为相机位置）
// =========================================================================
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

float4 PS(GSOutput pin) : SV_Target
{
    uint matIndex = gInstanceData[pin.InstanceIndex].MaterialIndex;

    MaterialData matData = gMaterialData[matIndex];

    float4 texColor = 1.0f;
    [branch] if (matData.BaseColorTexIndex != 0xFFFFFFFF)
    {
        texColor = gTextureMaps[matData.BaseColorTexIndex].Sample(gSamplerLinearWrap, pin.TexCoord);
    }
    float3 albedo = matData.BaseColor.rgb * texColor.rgb;
    float metallic = matData.Metallic;
    float roughness = matData.Roughness;
    float ao = matData.Ambient;
    float3 emissive = matData.Emissive.rgb * matData.Emissive.w;

    // PBR 贴图采样（替代固定值）
    [branch] if (matData.MetallicRoughnessTexIndex != 0xFFFFFFFF)
    {
        float2 mr = gTextureMaps[matData.MetallicRoughnessTexIndex].Sample(gSamplerLinearWrap, pin.TexCoord).rg;
        metallic = mr.r;
        roughness = mr.g;
    }
    [branch] if (matData.OcclusionTexIndex != 0xFFFFFFFF)
    {
        ao = gTextureMaps[matData.OcclusionTexIndex].Sample(gSamplerLinearWrap, pin.TexCoord).r;
    }

    float3 N = normalize(pin.WorldNormal);
    float4 normalSample = float4(0.5f, 0.5f, 1.0f, 1.0f);
    // 法线贴图
    [branch] if (matData.NormalTexIndex != 0xFFFFFFFF)
    {
        normalSample = gTextureMaps[matData.NormalTexIndex].Sample(gSamplerAnisotropicWrap, pin.TexCoord);
        normalSample.rgb = lerp(float3(0.5f, 0.5f, 1.0f), normalSample.rgb, matData.NormalStrength);
        N = NormalSampleToWorldSpace(normalSample.rgb, N, normalize(pin.WorldTangent));
    }
    float3 V = normalize(gProbePosition - pin.WorldPos);

    // 法线贴图 Alpha 通道调制粗糙度
    float roughnessMod = 1.0f - normalSample.a;
    roughness = saturate(roughness + roughnessMod * 0.3f);

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

    // 直接光照（无阴影）
    float3 directLight = 0;
    for (uint i = 0; i < gNumDirLights; ++i)
        directLight += ComputeDirectionalLight(gLights[i], mat, N, V);
    for (uint j = gNumDirLights; j < gNumDirLights + gNumPointLights; ++j)
        directLight += ComputePointLight(gLights[j], mat, pin.WorldPos, N, V);
    for (uint k = gNumDirLights + gNumPointLights; k < gNumDirLights + gNumPointLights + gNumSpotLights; ++k)
        directLight += ComputeSpotLight(gLights[k], mat, pin.WorldPos, N, V);

    // 无反射采样（探针捕获本身不能依赖探针）
    float3 litColor = ambient + directLight + emissive;

    return float4(litColor, mat.BaseColor.a);
}
