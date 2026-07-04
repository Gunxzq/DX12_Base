//***************************************************************************************
// LightingUtil.hlsl - PBR 版本
//***************************************************************************************

#ifndef LIGHTING_UTIL_HLSL
#define LIGHTING_UTIL_HLSL

#define MaxLights 16
#define PI 3.14159265359f

#ifndef NUM_DIR_LIGHTS
#define NUM_DIR_LIGHTS 1
#endif

#ifndef NUM_POINT_LIGHTS
#define NUM_POINT_LIGHTS 0
#endif

#ifndef NUM_SPOT_LIGHTS
#define NUM_SPOT_LIGHTS 0
#endif

struct Light
{
    float4 Strength;
    float4 Direction;
    float4 Position;
    float FalloffStart;
    float FalloffEnd;
    float SpotPower;
    float Range;
    float CastShadow;
    float ShadowBias;
    float ShadowMapIndex;
    float Pad;
};

struct Material
{
    float4 BaseColor;  // offset 0-15
    float Metallic;    // offset 16-19
    float Roughness;   // offset 20-23
    float Ambient;     // offset 24-27
    float Alpha;       // offset 28-31
    float4 Emissive;   // offset 32-47
    float AlphaCutoff; // offset 48-51
    float Padding[3];  // offset 52-63
};

// 法线分布函数 (GGX/Trowbridge-Reitz)
float DistributionGGX(float3 N, float3 H, float roughness)
{
    float a = roughness * roughness;
    float a2 = a * a;
    float NdotH = max(dot(N, H), 0.0f);
    float NdotH2 = NdotH * NdotH;

    float nom = a2;
    float denom = (NdotH2 * (a2 - 1.0f) + 1.0f);
    denom = PI * denom * denom;

    return nom / denom;
}

// 几何函数 (Smith-Schlick-GGX)
float GeometrySchlickGGX(float NdotV, float roughness)
{
    float r = (roughness + 1.0f);
    float k = (r * r) / 8.0f;
    float nom = NdotV;
    float denom = NdotV * (1.0f - k) + k;
    return nom / denom;
}

float GeometrySmith(float3 N, float3 V, float3 L, float roughness)
{
    float NdotV = max(dot(N, V), 0.0f);
    float NdotL = max(dot(N, L), 0.0f);
    float ggx2 = GeometrySchlickGGX(NdotV, roughness);
    float ggx1 = GeometrySchlickGGX(NdotL, roughness);
    return ggx1 * ggx2;
}

// 菲涅尔函数 (Schlick近似)
float3 FresnelSchlick(float cosTheta, float3 F0)
{
    return F0 + (1.0f - F0) * pow(1.0f - cosTheta, 5.0f);
}

// 从金属度和反照率计算 F0
float3 GetF0(float3 albedo, float metallic)
{
    return lerp(0.04f, albedo, metallic);
}

// 单光源 PBR 计算
float3 ComputePBR(Light L, Material mat, float3 pos, float3 normal, float3 toEye)
{
    float3 lightVec;
    float attenuation = 1.0f;
    float ndotl;
    float3 lightStrength = L.Strength.xyz;

    // 判断光源类型
    if (L.FalloffEnd > 0.0f)
    {
        // 点光源或聚光灯
        lightVec = L.Position.xyz - pos;
        float d = length(lightVec);
        if (d > L.FalloffEnd)
            return 0.0f;
        lightVec /= d;
        attenuation = saturate((L.FalloffEnd - d) / (L.FalloffEnd - L.FalloffStart));
        ndotl = max(dot(lightVec, normal), 0.0f);
        lightStrength *= ndotl * attenuation;

        // 聚光灯因子
        if (L.SpotPower > 0.0f)
        {
            float spotFactor = pow(max(dot(-lightVec, L.Direction.xyz), 0.0f), L.SpotPower);
            lightStrength *= spotFactor;
        }
    }
    else
    {
        // 方向光
        lightVec = -L.Direction.xyz;
        ndotl = max(dot(lightVec, normal), 0.0f);
        lightStrength *= ndotl;
    }

    if (ndotl <= 0.0f)
        return 0.0f;

    // 计算 PBR 所需中间值
    float3 albedo = mat.BaseColor.rgb;
    float metallic = mat.Metallic;
    float roughness = mat.Roughness;
    float ambient = mat.Ambient;

    float3 F0 = GetF0(albedo, metallic);
    float3 H = normalize(toEye + lightVec);
    float NdotV = max(dot(normal, toEye), 0.0f);
    float NdotH = max(dot(normal, H), 0.0f);
    float VdotH = max(dot(toEye, H), 0.0f);

    // 1. Cook-Torrance BRDF 高光项
    float NDF = DistributionGGX(normal, H, roughness);
    float G = GeometrySmith(normal, toEye, lightVec, roughness);
    float3 F = FresnelSchlick(VdotH, F0);

    float3 numerator = NDF * G * F;
    float denominator = 4.0f * NdotV * ndotl + 0.0001f;
    float3 specular = numerator / denominator;

    // 2. 漫反射项 (能量守恒)
    float3 kD = (1.0f - F) * (1.0f - metallic);
    float3 diffuse = kD * albedo / PI;

    // 3. 最终贡献
    return (diffuse + specular) * lightStrength;
}

// 方向光封装
float3 ComputeDirectionalLight(Light L, Material mat, float3 normal, float3 toEye)
{
    return ComputePBR(L, mat, float3(0, 0, 0), normal, toEye);
}

// 方向光 PBR（延迟渲染专用，参数来自 G-buffer，不含 Material 结构体）
float3 ComputeDirectionalLightDeferred(Light light, float3 albedo, float metallic, float roughness, float3 N, float3 V)
{
    float3 L = -light.Direction;
    float NdotL = max(dot(N, L), 0.0f);
    float3 H = normalize(V + L);
    float NdotH = max(dot(N, H), 0.0f);
    float NdotV = max(dot(N, V), 0.0f);
    float3 radiance = light.Strength;
    float3 F0 = lerp(0.04f, albedo, metallic);
    float3 F = F0 + (1.0f - F0) * pow(1.0f - NdotH, 5.0f);
    float a = roughness * roughness;
    float a2 = a * a;
    float NdotH2 = NdotH * NdotH;
    float D = a2 / (3.14159f * (NdotH2 * (a2 - 1.0f) + 1.0f) * (NdotH2 * (a2 - 1.0f) + 1.0f));
    float k = (roughness + 1.0f) * (roughness + 1.0f) / 8.0f;
    float G = (NdotV / (NdotV * (1.0f - k) + k)) * (NdotL / (NdotL * (1.0f - k) + k));
    float3 specular = D * G * F / (4.0f * max(NdotV, 0.01f) * max(NdotL, 0.01f) + 0.0001f);
    float3 kD = (1.0f - F) * (1.0f - metallic);
    float3 diffuse = kD * albedo / 3.14159f;
    return (diffuse + specular) * radiance * NdotL;
}

// 点光源封装
float3 ComputePointLight(Light L, Material mat, float3 pos, float3 normal, float3 toEye)
{
    return ComputePBR(L, mat, pos, normal, toEye);
}

// 聚光灯封装
float3 ComputeSpotLight(Light L, Material mat, float3 pos, float3 normal, float3 toEye)
{
    return ComputePBR(L, mat, pos, normal, toEye);
}

float4 ComputeLighting(Light gLights[MaxLights], Material mat,
                       float3 pos, float3 normal, float3 toEye,
                       float3 shadowFactor)
{
    float3 result = 0.0f;

#if (NUM_DIR_LIGHTS > 0)
    for (int i = 0; i < NUM_DIR_LIGHTS; ++i)
    {
        result += shadowFactor[i] * ComputeDirectionalLight(gLights[i], mat, normal, toEye);
    }
#endif

#if (NUM_POINT_LIGHTS > 0)
    for (int i = NUM_DIR_LIGHTS; i < NUM_DIR_LIGHTS + NUM_POINT_LIGHTS; ++i)
    {
        result += ComputePointLight(gLights[i], mat, pos, normal, toEye);
    }
#endif

#if (NUM_SPOT_LIGHTS > 0)
    for (int i = NUM_DIR_LIGHTS + NUM_POINT_LIGHTS; i < NUM_DIR_LIGHTS + NUM_POINT_LIGHTS + NUM_SPOT_LIGHTS; ++i)
    {
        result += ComputeSpotLight(gLights[i], mat, pos, normal, toEye);
    }
#endif

    return float4(result, 0.0f);
}

#endif // LIGHTING_UTIL_HLSL