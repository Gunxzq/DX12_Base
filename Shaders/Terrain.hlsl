// Terrain.hlsl
//==============================================================================
// 地形渲染着色器 - 支持曲面细分 (Tessellation)
//   不包含 Common_PBR.hlsl，避免 cbPerObject 重定义冲突
//   手动声明所有共享资源，槽位与 color.hlsl 完全一致
//   相比 color.hlsl 仅多了 VS→HS→DS 曲面细分管线
//==============================================================================

#include "LightingUtil.hlsl"

SamplerComparisonState gShadowSampler : register(s11);

#include "ShadowSampling.hlsl"

// ============================================================================
// 材质数据（与 Common_PBR.hlsl 一致，但需要手动声明）
// ============================================================================
struct MaterialData
{
    float4 BaseColor;
    float Metallic;
    float Roughness;
    float Ambient;
    float Alpha;
    float4 Emissive;
    float AlphaCutoff;
    uint BaseColorTexIndex;
    uint NormalTexIndex;
    uint MetallicRoughnessTexIndex;
    uint EmissiveTexIndex;
    uint OcclusionTexIndex;
    float MatPad[2];
};

// ============================================================================
// 常量缓冲 — 槽位与 Common_PBR.hlsl / color.hlsl 完全一致
//   b0: cbPerObject（地形扩展版）
//   b1: cbPass（与 color 完全相同）
//   b2: cbLights（与 color 完全相同）
// ============================================================================

// ---- cbPerObject (b0) - 地形扩展版 ----
// 相比 color.hlsl 的 cbPerObject，追加了地形专用字段
cbuffer cbPerObject : register(b0)
{
    row_major float4x4 gWorld;
    row_major float4x4 gWorldInvTrans;
    row_major float4x4 gPrevWorld;
    uint gMaterialIndex;
    uint gReceiveShadow;
    float2 gObjPad;

    // 地形专用参数
    float gHeightScale;
    float gHeightOffset;
    float gTessellationFactor;
    float gTessellationDistanceMin;
    float gTessellationDistanceMax;
    uint gHeightMapIndex;
    uint gAlbedoMapIndex;
    uint gNormalMapIndex;
    float gTerrainPad;
    float gPadaa[3];
}

// ---- cbPass (b1) - 与 Common_PBR.hlsl 完全一致 ----
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
    float4 gPaddwdw[3];
}

// ---- cbLights (b2) - 与 Common_PBR.hlsl 完全一致 ----
cbuffer cbLights : register(b2)
{
    Light gLights[256];
    float4 gAmbientLight;
    uint gNumDirLights;
    uint gNumPointLights;
    uint gNumSpotLights;
    uint gLightsPad[5];
}

// ============================================================================
// 资源绑定 — 槽位与 Common_PBR.hlsl / color.hlsl 完全一致
//   t0,space1: StructuredBuffer<MaterialData>
//   t0:        gTexture / gTerrainTextures（color 用单个纹理，地形用纹理数组）
//   t10:       gEnvMap
//   s0~s5:     采样器
//   s10:       gEnvSampler
//   s11:       gShadowSampler
// ============================================================================

StructuredBuffer<MaterialData> gMaterialData : register(t0, space1);
TextureCube gEnvMap : register(t10);
SamplerState gEnvSampler : register(s10);

SamplerState gSamplerPointWrap : register(s0);
SamplerState gSamplerPointClamp : register(s1);
SamplerState gSamplerLinearWrap : register(s2);
SamplerState gSamplerLinearClamp : register(s3);
SamplerState gSamplerAnisotropicWrap : register(s4);
SamplerState gSamplerAnisotropicClamp : register(s5);
// gSampler = gSamplerLinearWrap（直接使用 gSamplerLinearWrap，无需别名）

// ---- 地形纹理数组 (t0~t7, space0) ----
// 使用固定大小数组避免与 ShadowSampling 中 space1 的无界数组冲突
// color.hlsl: gTexture : register(t0)
Texture2D gTerrainTextures[8] : register(t0, space0);

// ============================================================================
// 环境反射（与 Common_PBR.hlsl 一致）
// ============================================================================
float3 ComputeEnvironmentReflection(float3 reflectDir, float3 albedo, float metallic, float roughness, float3 N, float3 V)
{
    float3 reflection = gEnvMap.Sample(gEnvSampler, reflectDir).rgb;
    float3 F0 = lerp(0.04f, albedo, metallic);
    float NdotV = max(dot(N, V), 0.0f);
    float3 fresnel = FresnelSchlick(NdotV, F0);
    float strength = 1.0f - roughness * 0.5f;
    return reflection * fresnel * strength;
}

// ============================================================================
// 顶点输入（与 color.hlsl VertexIn 一致）
// ============================================================================
struct VertexIn
{
    float3 PosL : POSITION;
    float3 NormalL : NORMAL;
    float3 TangentL : TANGENT;
    float2 TexCoord : TEXCOORD;
};

// ============================================================================
// 曲面细分管线结构体
// ============================================================================
struct HullControlPoint
{
    float3 PosL : POSITION;
    float3 NormalL : NORMAL;
    float3 TangentL : TANGENT;
    float2 TexCoord : TEXCOORD;
};

struct HullConstantOutput
{
    float EdgeTessFactor[4] : SV_TessFactor;
    float InsideTessFactor[2] : SV_InsideTessFactor;
};

// ---- Domain Shader 输出（与 color.hlsl VertexOut 语义一致） ----
struct DomainOutput
{
    float4 PosH : SV_POSITION;
    float3 WorldPos : POSITION;
    float3 WorldNormal : NORMAL;
    float3 WorldTangent : TANGENT;
    float2 TexCoord : TEXCOORD;
};

// ============================================================================
// Vertex Shader - 透传控制点到 Hull Shader
//   地形不需要实例化，使用标准 VS
// ============================================================================
HullControlPoint VS(VertexIn vin)
{
    HullControlPoint vout;
    vout.PosL = vin.PosL;
    vout.NormalL = vin.NormalL;
    vout.TangentL = vin.TangentL;
    vout.TexCoord = vin.TexCoord;
    return vout;
}

// ============================================================================
// 常量 Hull Shader - 根据面片到相机的距离计算细分因子 (LOD)
// ============================================================================
HullConstantOutput ConstantHS(InputPatch<HullControlPoint, 4> patch, uint patchID : SV_PrimitiveID)
{
    HullConstantOutput output;

    // 计算面片中心的世界坐标
    float3 centerL = (patch[0].PosL + patch[1].PosL + patch[2].PosL + patch[3].PosL) * 0.25f;
    float3 centerW = mul(float4(centerL, 1.0f), gWorld).xyz;

    // 距离自适应 LOD 细分
    //   distance <  Min  → 使用最大细分因子 (gTessellationFactor)
    //   distance >= Max  → 不细分 (1.0)
    //   Min <= dist < Max → 线性插值
    float distance = length(centerW - gCameraPos);

    float tess;
    if (distance < gTessellationDistanceMin)
        tess = gTessellationFactor;
    else if (distance > gTessellationDistanceMax)
        tess = 1.0f;
    else
        tess = lerp(gTessellationFactor, 1.0f,
                    (distance - gTessellationDistanceMin) / (gTessellationDistanceMax - gTessellationDistanceMin));

    // 钳制范围（D3D12 支持的最大细分因子为 64）
    tess = clamp(tess, 1.0f, 64.0f);

    output.EdgeTessFactor[0] = tess;
    output.EdgeTessFactor[1] = tess;
    output.EdgeTessFactor[2] = tess;
    output.EdgeTessFactor[3] = tess;
    output.InsideTessFactor[0] = tess;
    output.InsideTessFactor[1] = tess;

    return output;
}

// ============================================================================
// Hull Shader - 传递控制点
// ============================================================================
[domain("quad")]
    [partitioning("fractional_even")]
    [outputtopology("triangle_cw")]
    [outputcontrolpoints(4)]
    [patchconstantfunc("ConstantHS")] HullControlPoint
    HS(InputPatch<HullControlPoint, 4> patch, uint id : SV_OutputControlPointID)
{
    return patch[id];
}

    // ============================================================================
    // Domain Shader - 顶点置换 + 世界空间变换
    //   输出的 DomainOutput 与 color.hlsl 的 VertexOut 语义一致
    // ============================================================================
    [domain("quad")] DomainOutput DS(HullConstantOutput input, float2 uv : SV_DomainLocation, const OutputPatch<HullControlPoint, 4> patch)
{
    DomainOutput output;

    float3 v1 = lerp(patch[0].PosL, patch[1].PosL, uv.x); // 上边：左上→右上
    float3 v2 = lerp(patch[2].PosL, patch[3].PosL, uv.x); // 下边：左下→右下
    float3 localPos = lerp(v1, v2, uv.y);                 // 纵向：远→近（上→下）

    // 2. 双线性插值 UV（必须在高度采样之前）
    float2 texCoord = lerp(
        lerp(patch[0].TexCoord, patch[1].TexCoord, uv.x),
        lerp(patch[2].TexCoord, patch[3].TexCoord, uv.x), uv.y);

    // 3. 采样高度图进行顶点置换
    float height = gTerrainTextures[gHeightMapIndex].SampleLevel(gSamplerLinearWrap, texCoord, 0).r;
    localPos.y = height * gHeightScale + gHeightOffset;

    // 4. 双线性插值法线
    float3 normal = normalize(lerp(
        lerp(patch[0].NormalL, patch[1].NormalL, uv.x),
        lerp(patch[2].NormalL, patch[3].NormalL, uv.x), uv.y));

    // 5. 双线性插值切线
    float3 tangent = normalize(lerp(
        lerp(patch[0].TangentL, patch[1].TangentL, uv.x),
        lerp(patch[2].TangentL, patch[3].TangentL, uv.x), uv.y));

    // 6. 世界空间 + 裁剪空间变换（与 color.hlsl VS 输出一致）
    float4 worldPos = mul(float4(localPos, 1.0f), gWorld);
    output.WorldPos = worldPos.xyz;
    output.PosH = mul(worldPos, gViewProj);
    output.WorldNormal = normalize(mul(normal, (float3x3)gWorldInvTrans));
    output.WorldTangent = normalize(mul(tangent, (float3x3)gWorld));
    output.TexCoord = texCoord;

    return output;
}

// ============================================================================
// Pixel Shader - 与 color.hlsl PS 保持一致的光照逻辑
//   不同之处：纹理来自 gTerrainTextures 数组而非单个 gTexture
// ============================================================================
float4 PS(DomainOutput pin) : SV_Target
{
    // 采样漫反射纹理
    float4 texColor = gTerrainTextures[gAlbedoMapIndex].Sample(gSamplerLinearWrap, pin.TexCoord);

    float3 albedo = texColor.rgb;
    float metallic = 0.0f;
    float roughness = 0.8f;
    float ao = 0.5f;
    float3 emissive = float3(0, 0, 0);

    // 法线
    float3 N = normalize(pin.WorldNormal);
    if (gNormalMapIndex != 0xFFFFFFFF)
    {
        float3 normalMap = gTerrainTextures[gNormalMapIndex].Sample(gSamplerLinearWrap, pin.TexCoord).xyz;
        normalMap = normalMap * 2.0f - 1.0f;
        N = normalize(normalMap);
    }

    float3 V = normalize(gCameraPos - pin.WorldPos);

    Material mat;
    mat.BaseColor = float4(albedo, 1.0f);
    mat.Metallic = metallic;
    mat.Roughness = roughness;
    mat.Ambient = ao;
    mat.Emissive = float4(emissive, 1.0f);
    mat.Alpha = 1.0f;
    mat.AlphaCutoff = 0.0f;

    // 环境光
    float3 ambient = gAmbientLight.xyz * gAmbientLight.w * albedo * ao;

    // 直接光照（带阴影，与 color.hlsl 一致）
    float3 directLight = 0;
    for (uint i = 0; i < gNumDirLights; ++i)
    {
        float3 lightContrib = ComputeDirectionalLight(gLights[i], mat, N, V);

        if (gLights[i].ShadowMapIndex >= 0 && gReceiveShadow)
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
    float3 R = reflect(-V, N);
    float3 reflection = ComputeEnvironmentReflection(R, albedo, metallic, roughness, N, V);

    float3 litColor = ambient + directLight + reflection + emissive;

    return float4(litColor, 1.0f);
}
