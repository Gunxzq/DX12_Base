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
    float NormalStrength;

    // 贴图索引（0xFFFFFFFF = 无效）
    uint BaseColorTexIndex;
    uint NormalTexIndex;
    uint MetallicRoughnessTexIndex;
    uint EmissiveTexIndex;
    uint OcclusionTexIndex;
    uint HeightTexIndex;
    uint OpacityTexIndex;
    uint MaskTexIndex;
    uint SubsurfaceTexIndex;
    uint ClearCoatTexIndex;
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
    float gHeightScale;
    float gHeightOffset;
    float gTessellationFactor;
    float gTessellationDistanceMin;
    float gTessellationDistanceMax;
    uint gHeightMapIndex;
    uint gAlbedoMapIndex;
    uint gNormalMapIndex;
    float gTerrainPad;
    float padsss[5];
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
// 资源绑定 — 槽位与 Common_PBR.hlsl / color.hlsl 保持一致
//   t0,space1: StructuredBuffer<MaterialData>
//   t0,space0: gTerrainTextures（地形纹理数组）
//   t10,space0: gEnvMap
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
Texture2D gTerrainTextures[3] : register(t0, space0);

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
//   - 使用 fraction_odd 细分模式，消除顶点跳变（蠕动）
//   - 背面剔除：背对相机的面片直接不细分
//   - 迟滞区间 (Hysteresis)：避免相机微小移动导致 LOD 跳变
// ============================================================================
HullConstantOutput ConstantHS(InputPatch<HullControlPoint, 4> patch, uint patchID : SV_PrimitiveID)
{
    HullConstantOutput output;

    // 计算面片中心的世界坐标
    float3 centerL = (patch[0].PosL + patch[1].PosL + patch[2].PosL + patch[3].PosL) * 0.25f;
    float3 centerW = mul(float4(centerL, 1.0f), gWorld).xyz;

    // 背面剔除：计算面片法线，如果背对相机则不细分
    float3 edgeA = patch[1].PosL - patch[0].PosL;
    float3 edgeB = patch[2].PosL - patch[0].PosL;
    float3 faceNormal = normalize(cross(edgeA, edgeB));
    float3 faceCenterW = mul(float4(centerL, 1.0f), gWorld).xyz;
    float3 viewDir = normalize(gCameraPos - faceCenterW);
    float ndotv = dot(faceNormal, mul(viewDir, (float3x3)gWorld));

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

    // 迟滞区间 (Hysteresis)：在过渡边界做 10% 的模糊，避免 LOD 频繁切换
    float hysteresis = 0.1f * (gTessellationDistanceMax - gTessellationDistanceMin);
    float range = gTessellationDistanceMax - gTessellationDistanceMin;
    if (distance > gTessellationDistanceMin - hysteresis && distance < gTessellationDistanceMin + hysteresis)
    {
        // 在过渡区间做平滑
        float t = (distance - (gTessellationDistanceMin - hysteresis)) / (2.0f * hysteresis);
        tess = lerp(gTessellationFactor, tess, smoothstep(0.0f, 1.0f, t));
    }

    // 背面剔除：背向相机超过 85° 的面片不细分
    if (ndotv < -0.08f)
    {
        tess = 1.0f;
    }

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
//   partitioning("fractional_odd") — 平滑插入/移除顶点，消除蠕动
// ============================================================================
[domain("quad")]
    [partitioning("fractional_odd")]
    [outputtopology("triangle_cw")]
    [outputcontrolpoints(4)]
    [patchconstantfunc("ConstantHS")] HullControlPoint
    HS(InputPatch<HullControlPoint, 4> patch, uint id : SV_OutputControlPointID)
{
    return patch[id];
}

    // ============================================================================
    // Domain Shader - 顶点置换 + 法线重建 + 世界空间变换
    //   - 控制点 PosL.y 来自 PNG 高度图（粗网格 257x257）
    //   - DDS 纹理（高分辨率）提供精细化偏移，叠加到插值后的粗网格高度上
    //   - 用有限差分法采样高度图重建真实法线（解决棱角）
    //   - 输出的 DomainOutput 与 color.hlsl VertexOut 语义一致
    // ============================================================================
    [domain("quad")] DomainOutput DS(HullConstantOutput input, float2 uv : SV_DomainLocation, const OutputPatch<HullControlPoint, 4> patch)
{
    DomainOutput output;

    // D3D quad domain: patch[0]=(0,0)左上, patch[1]=(1,0)右上, patch[2]=(0,1)左下, patch[3]=(1,1)右下
    // TerrainLoader 索引顺序: i0(左上), i1(右上), i2(左下), i3(右下) — 与 D3D 一致

    // 1. 双线性插值 UV
    float2 texCoord = lerp(
        lerp(patch[0].TexCoord, patch[1].TexCoord, uv.x),
        lerp(patch[2].TexCoord, patch[3].TexCoord, uv.x), uv.y);

    // 2. 双线性插值位置（XZ + 粗粒度高度来自 CPU 端 PNG 采样结果）
    float3 v1 = lerp(patch[0].PosL, patch[1].PosL, uv.x); // 上边：左上→右上
    float3 v2 = lerp(patch[2].PosL, patch[3].PosL, uv.x); // 下边：左下→右下
    float3 localPos = lerp(v1, v2, uv.y);

    // 3. 从高分辨率 DDS 纹理采样精细细节偏移（5 点平滑，消除高频噪点尖刺）
    //    对中心点及上下左右 4 个相邻点取平均，模糊化像素间剧烈跳变
    float texelU = 1.0f / 512.0f; // 高度图纹理像素步长（H_Runtime_heightmap 512 分辨率）
    float texelV = 1.0f / 512.0f;

    float h0 = gTerrainTextures[gHeightMapIndex].SampleLevel(gSamplerLinearWrap, texCoord, 0).r;
    float hU0 = gTerrainTextures[gHeightMapIndex].SampleLevel(gSamplerLinearWrap, texCoord + float2(texelU, 0), 0).r;
    float hU1 = gTerrainTextures[gHeightMapIndex].SampleLevel(gSamplerLinearWrap, texCoord - float2(texelU, 0), 0).r;
    float hV0 = gTerrainTextures[gHeightMapIndex].SampleLevel(gSamplerLinearWrap, texCoord + float2(0, texelV), 0).r;
    float hV1 = gTerrainTextures[gHeightMapIndex].SampleLevel(gSamplerLinearWrap, texCoord - float2(0, texelV), 0).r;
    float detailHeight = (h0 + hU0 + hU1 + hV0 + hV1) / 5.0f;

    float detailOffset = (detailHeight - 0.5f) * gHeightScale + gHeightOffset;
    localPos.y += detailOffset;

    // 4. ★ 法线重建（有限差分法）— 解决棱角问题的关键
    //    用较大步长采样左右/上下高度差，避免高频噪点导致法线抖动
    float du = texelU * 2.0f; // 跨 2 像素步长，降低噪声敏感度
    float dv = texelV * 2.0f;

    float hL = gTerrainTextures[gHeightMapIndex].SampleLevel(gSamplerLinearWrap, texCoord + float2(-du, 0), 0).r;
    float hR = gTerrainTextures[gHeightMapIndex].SampleLevel(gSamplerLinearWrap, texCoord + float2(+du, 0), 0).r;
    float hD = gTerrainTextures[gHeightMapIndex].SampleLevel(gSamplerLinearWrap, texCoord + float2(0, -dv), 0).r;
    float hU = gTerrainTextures[gHeightMapIndex].SampleLevel(gSamplerLinearWrap, texCoord + float2(0, +dv), 0).r;

    // 高度图在世界空间的步长
    //   UV 空间 1.0 = 256 world units
    float worldTexelSize = 256.0f;

    float dh_dx = (hR - hL) * gHeightScale / (2.0f * du * worldTexelSize);
    float dh_dz = (hU - hD) * gHeightScale / (2.0f * dv * worldTexelSize);

    // 切线方向 (XZ 平面)
    float3 tangentX = float3(1.0f, dh_dx, 0.0f);
    float3 tangentZ = float3(0.0f, dh_dz, 1.0f);

    // 法线 = cross(tangentZ, tangentX)，确保朝上
    float3 normal = normalize(cross(tangentZ, tangentX));
    // 如果法线朝下则翻转
    if (normal.y < 0.0f)
        normal = -normal;

    // 5. 切线（保持与原始几何一致的 TBN）
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
    float roughness = 0.9f; // 雪原高粗糙度
    float ao = 0.5f;
    float3 emissive = float3(0, 0, 0);

    // 法线（含法线贴图 TBN 变换）
    float3 N = normalize(pin.WorldNormal);
    if (gNormalMapIndex != 0xFFFFFFFF)
    {
        float3 normalMap = gTerrainTextures[gNormalMapIndex].Sample(gSamplerLinearWrap, pin.TexCoord).xyz;
        float3 normalT = 2.0f * normalMap - 1.0f;

        float3 T = normalize(pin.WorldTangent - N * dot(pin.WorldTangent, N));
        float3 B = cross(N, T);
        float3x3 TBN = float3x3(T, B, N);
        N = normalize(mul(normalT, TBN));
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
    uint totalLights = gNumDirLights + gNumPointLights + gNumSpotLights;
    for (uint i = 0; i < totalLights; ++i)
    {
        Light light = gLights[i];
        float3 lightContrib = 0;

        [branch]
        if (light.Type == 0) // Directional
        {
            lightContrib = ComputeDirectionalLight(light, mat, N, V);

            if (light.ShadowMapIndex >= 0 && gReceiveShadow)
            {
                lightContrib *= SampleShadow(light, pin.WorldPos, N);
            }
        }
        else if (light.Type == 1) // Point
        {
            lightContrib = ComputePointLight(light, mat, pin.WorldPos, N, V);
        }
        else // Spot
        {
            lightContrib = ComputeSpotLight(light, mat, pin.WorldPos, N, V);
        }

        directLight += lightContrib;
    }

    // 环境反射
    float3 R = reflect(-V, N);
    float3 reflection = ComputeEnvironmentReflection(R, albedo, metallic, roughness, N, V);

    float3 litColor = ambient + directLight + reflection + emissive;

    return float4(litColor, 1.0f);
}

// ========================================================================
// PS_GBuffer — 地形 G-buffer MRT 输出（复用 Domain Shader）
// ========================================================================
struct GBufferOutput {
    float4 Albedo   : SV_Target0;
    float4 Normal   : SV_Target1;
    float4 Material : SV_Target2;
    float4 WorldPos : SV_Target3;
};

GBufferOutput PS_GBuffer(DomainOutput pin) {
    GBufferOutput output = (GBufferOutput)0.0f;

    float4 texColor = gTerrainTextures[gAlbedoMapIndex].Sample(gSamplerLinearWrap, pin.TexCoord);
    float3 albedo = texColor.rgb;
    float metallic = 0.0f;
    float roughness = 0.9f; // 雪原高粗糙度
    float ao = 0.5f;

    // 法线（含法线贴图 TBN 变换）
    float3 N = normalize(pin.WorldNormal);
    if (gNormalMapIndex != 0xFFFFFFFF) {
        float3 normalMap = gTerrainTextures[gNormalMapIndex].Sample(gSamplerLinearWrap, pin.TexCoord).xyz;
        float3 normalT = 2.0f * normalMap - 1.0f;
        float3 T = normalize(pin.WorldTangent - N * dot(pin.WorldTangent, N));
        float3 B = cross(N, T);
        float3x3 TBN = float3x3(T, B, N);
        N = normalize(mul(normalT, TBN));
    }

    output.Albedo = float4(albedo, 1.0f);
    output.Normal = float4(N * 0.5f + 0.5f, 1.0f);
    output.Material = float4(metallic, roughness, ao, 0.0f);
    output.WorldPos = float4(pin.WorldPos, 1.0f);
    return output;
}
