// ========================================================================
// preview.hlsl — 资产预览 PBR 前向着色器
//
// 使用材质标志（BaseColorTexIndex != 0xFFFFFFFF）控制纹理采样，
// 摒弃 hasTexture 分支。支持 PBR 材质参数 + 单方向光渲染。
// 仅用于编辑器资产预览（单个物体，独立 RT）。
// ========================================================================

#include "LightingUtil.hlsl"

// ========================================================================
// 输入布局
// ========================================================================

struct VSInput {
    float3 position : POSITION;
    float3 normal : NORMAL;
    float3 tangent : TANGENT;
    float2 texcoord : TEXCOORD;
};

struct VSOutput {
    float4 position : SV_POSITION;
    float3 worldPos : POSITION;
    float3 worldNormal : NORMAL;
    float3 worldTangent : TANGENT;
    float2 texcoord : TEXCOORD;
};

// ========================================================================
// 材质数据（与 Common_PBR.hlsl 的 MaterialData 对齐）
// ========================================================================

struct PreviewMaterial {
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
    uint _pad[3];
};

// ========================================================================
// 常量缓冲（256 bytes，对齐 D3D12 要求）
// ========================================================================

cbuffer PreviewCB : register(b0) {
    row_major float4x4 gWorldViewProj;  // 64 bytes
    row_major float4x4 gWorld;          // 64 bytes
    float4 gCameraPos;                  // 16 bytes (w unused)
    float4 gLightDirection;             // 16 bytes (w unused)
    float4 gLightStrength;              // 16 bytes (w unused)
    float4 gBaseColor;                  // 16 bytes
    float4 gEmissive;                   // 16 bytes
    float4 gMaterialParams;             // x=metallic, y=roughness, z=ambient, w=alpha
    uint4 gTexIndices;                  // x=baseColorTexIndex, y=normalTexIndex, z=metallicRoughnessTexIndex, w=occlusionTexIndex
    float4 gExtra;                      // x=alphaCutoff, y=normalStrength, z=emissiveTexIndex, w=unused
}

// ========================================================================
// 资源绑定
// ========================================================================

Texture2D gPreviewTexture : register(t0);
SamplerState gPreviewSampler : register(s0);

// ========================================================================
// 法线贴图 TBN 变换
// ========================================================================

float3 NormalSampleToWorldSpace(float3 normalMapSample, float3 unitNormalW, float3 tangentW) {
    float3 normalT = 2.0f * normalMapSample - 1.0f;
    float lenSq = dot(normalT.xy, normalT.xy);
    normalT.z = sqrt(max(0.0f, 1.0f - lenSq));
    float3 N = unitNormalW;
    float3 T = normalize(tangentW - dot(tangentW, N) * N);
    float3 B = cross(N, T);
    return mul(normalT, float3x3(T, B, N));
}

// ========================================================================
// 顶点着色器
// ========================================================================

VSOutput VS(VSInput input) {
    VSOutput output;
    float4 worldPos = mul(float4(input.position, 1.0f), gWorld);
    output.worldPos = worldPos.xyz;
    output.position = mul(worldPos, gWorldViewProj);
    output.worldNormal = normalize(mul(input.normal, (float3x3)gWorld));
    output.worldTangent = normalize(mul(input.tangent, (float3x3)gWorld));
    output.texcoord = input.texcoord;
    return output;
}

// ========================================================================
// 像素着色器 — PBR 前向渲染
// ========================================================================

float4 PS(VSOutput input) : SV_TARGET {
    // ── 采样纹理（使用材质标志，类似 color.hlsl 的 BaseColorTexIndex 模式） ──
    float4 texColor = 1.0f;
    [branch] if (gTexIndices.x != 0xFFFFFFFF) {
        texColor = gPreviewTexture.Sample(gPreviewSampler, input.texcoord);
    }

    float3 albedo = gBaseColor.rgb * texColor.rgb;
    float metallic = gMaterialParams.x;
    float roughness = gMaterialParams.y;
    float ao = gMaterialParams.z;
    float alpha = gMaterialParams.w;

    // ── 法线 ──
    float3 N = normalize(input.worldNormal);
    [branch] if (gTexIndices.y != 0xFFFFFFFF) {
        // 法线贴图需额外纹理绑定，预览暂不启用
        // 预留扩展点
    }

    // ── 光照方向 ──
    float3 V = normalize(gCameraPos.xyz - input.worldPos);
    // gLightDirection 是光照射方向（light-to-surface，如 {0,-1,0} 从上往下）
    // ComputePBR 内部会取反得到 surface-to-light 方向用于 NdotL 计算
    float3 L = normalize(gLightDirection.xyz);

    // ── 方向光 PBR ──
    Material mat;
    mat.BaseColor = float4(albedo, 1.0f);
    mat.Metallic = metallic;
    mat.Roughness = roughness;
    mat.Ambient = ao;
    mat.Alpha = alpha;
    mat.Emissive = gEmissive;
    mat.AlphaCutoff = gExtra.x;

    Light dirLight;
    dirLight.Strength = gLightStrength;
    dirLight.Direction = float4(L, 0.0f);
    dirLight.FalloffEnd = 0.0f; // 方向光
    dirLight.Type = 0;

    float3 direct = ComputeDirectionalLight(dirLight, mat, N, V);

    // ── 环境反射（简化版） ──
    float3 F0 = GetF0(albedo, metallic);
    float3 reflectDir = reflect(-V, N);
    float3 envColor = float3(0.25f, 0.28f, 0.32f);
    float NdotV = max(dot(N, V), 0.0f);
    float3 fresnel = FresnelSchlick(NdotV, F0);
    float3 reflection = envColor * fresnel * (1.0f - roughness * 0.5f);

    // ── 半球环境光 ──
    float3 ambient = 0.03f * albedo * ao;

    // ── 合成输出 ──
    float3 finalColor = ambient + direct + reflection;

    // 阿尔法裁剪
    if (alpha < gExtra.x)
        discard;

    return float4(finalColor, 1.0f);
}