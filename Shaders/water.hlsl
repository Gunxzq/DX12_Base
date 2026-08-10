// water.hlsl - 水面着色器（材质槽模式）
#include "Common_PBR.hlsl"

// 水纹理通过材质系统的纹理堆访问，不再使用硬编码 t0

// 场景深度（岸线渐隐用，根签名 slot 7 绑定，D32→R32_FLOAT SRV）
Texture2D gSceneDepth : register(t11);

// NDC 深度 → 线性 view-space Z（DX 透视投影：z/w，负数越远越负）
// ps_5_1 不支持 lambda，用文件级辅助函数
float LinearDepthFromNDC(float2 ndcXY, float ndcZ)
{
    float4 p = mul(float4(ndcXY, ndcZ, 1.0f), gInvProj);
    return p.z / p.w;
}

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
    float gFadeRange;             // 岸线渐隐距离（深度空间，gPad1 复用）
    float gUVTiling;              // 世界 UV 平铺（worldPos.xz * gUVTiling，纹理跨块连续）
    float gPad2;                  // 填充
    float gPad3;                  // 填充
    float gPad4;                  // 填充（16 字段 = 64B，对齐 C++ WaterConstants）
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

    // 波形必须使用世界坐标（同一世界位置在任何水块上算出一致的位移，
    // 否则多块水面边界相位断裂 → 运行时缝隙）。先算世界坐标再求波形。
    float4 worldPos = mul(float4(vin.PosL, 1.0f), gWorld);

    float time = gTotalTime * gWaveSpeed;

    float y = 0.0f;
    y += sin(worldPos.x * gWaveFrequency + time) * cos(worldPos.z * gWaveFrequency * 0.8f + time * 0.7f) * gWaveAmplitude;
    y += sin(worldPos.x * gWaveFrequency * 2.0f - time * 1.3f) * 0.3f * gWaveAmplitude;
    y += cos(worldPos.z * gWaveFrequency * 1.5f + time * 0.9f) * 0.2f * gWaveAmplitude;
    y += sin((worldPos.x * 0.5f + worldPos.z * 0.5f) * gWaveFrequency * 1.2f + time * 1.1f) * 0.15f * gWaveAmplitude;

    float3 posL = vin.PosL;
    posL.y += y;

    worldPos = mul(float4(posL, 1.0f), gWorld);
    vout.WorldPos = worldPos.xyz;
    vout.PosH = mul(worldPos, gViewProj);

    float3 normalL = vin.NormalL;
    normalL.y += y * 0.5f;
    vout.WorldNormal = normalize(mul(normalL, (float3x3)gWorldInvTrans));
    vout.WorldTangent = normalize(mul(vin.TangentL, (float3x3)gWorld));
    // 世界坐标 UV（跨块连续——同一世界位置在任何水块上采样到相同纹素，
    // 对齐波形世界坐标；gUVTiling 控制平铺密度，纹理 WRAP 平铺不受块边界影响）
    vout.TexCoord = worldPos.xz * gUVTiling;

    return vout;
}

float4 PS(VertexOut pin) : SV_Target
{
    MaterialData matData = gMaterialData[gMaterialIndex];

    // 岸线深度渐隐（UE DepthFade 同款）：采样场景深度，浅水区透明度衰减
    // gFadeRange <= 0 时禁用（不绑定深度 SRV 的降级路径）
    //
    // 深度缓冲是 NDC 深度（1/z 非线性，远处≈1、近处≈0），直接用差值计算水深
    // 会在远距离被压缩失真（水底/地面深度差极小 → shoreFade 恒满）。必须先经
    // gInvProj 还原为线性 view-space Z 再求差（对齐 UE DepthFade 线性化）。
    float shoreFade = 1.0f;
    [branch] if (gFadeRange > 0.0f)
    {
        uint sw, sh;
        gSceneDepth.GetDimensions(sw, sh);
        float2 depthUV = pin.PosH.xy / float2(float(sw), float(sh));
        float2 ndcXY = depthUV * 2.0f - 1.0f;

        float sceneDepth = gSceneDepth.Sample(gSamplerPointClamp, depthUV).r;
        float sceneLinear = LinearDepthFromNDC(ndcXY, sceneDepth);
        float waterLinear = LinearDepthFromNDC(ndcXY, pin.PosH.z);
        // 水深 = |场景线性深度 - 水面线性深度|（水面在场景上方时场景更远，取绝对值）
        float waterDepth = abs(sceneLinear - waterLinear);
        shoreFade = saturate(waterDepth / gFadeRange);
    }

    float2 texCoord = pin.TexCoord;
    texCoord.x += gTotalTime * 0.1f;
    texCoord.y += gTotalTime * 0.05f;

    // 材质纹理通过 gTextureMaps[] 采样（与 PBR 着色器一致）
    float4 texColor = 1.0f;
    [branch] if (matData.BaseColorTexIndex != 0xFFFFFFFF)
        texColor = gTextureMaps[matData.BaseColorTexIndex].Sample(gSamplerAnisotropicWrap, texCoord);
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
    uint totalLights = gNumDirLights + gNumPointLights + gNumSpotLights;
    for (uint i = 0; i < totalLights; ++i)
        directLight += ComputePBR(gLights[i], mat, pin.WorldPos, N, V);
    float3 R = reflect(-V, N);
    float3 reflection = ComputeEnvironmentReflection(R, albedo, metallic, roughness, N, V);

    // 使用 WaterConstants 中的参数
    float waterReflectionStrength = 0.6f;
    float waterDiffuseStrength = 0.3f;
    // 泡沫效果（基于波浪高度 y，只在波峰出现）
    float foam = saturate(pin.WorldPos.y - 9.8f) * gFoamIntensity;
    float3 foamColor = float3(0.9f, 0.9f, 0.8f);

    // 光照合成：ambient + 直射光 + 环境反射（天空盒采样）+ 自发光
    // reflection 此前计算但未加入 litColor——水面发黑主因。水是高反射材质，
    // 环境反射权重高于漫反射（菲涅尔驱动，见 waterDiffuseStrength 削弱直射）。
    float3 litColor = ambient + directLight * waterDiffuseStrength + reflection * waterReflectionStrength + emissive;

    // 叠加泡沫
    litColor = lerp(litColor, foamColor, foam * 0.3f);

    // 岸线渐隐应用到最终透明度（浅水区透明露出地面）
    float alpha = mat.BaseColor.a * shoreFade;
    return float4(litColor, alpha);
}