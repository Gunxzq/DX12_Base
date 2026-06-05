

#include "Common_PBR.hlsl"

// ============================================================================
// 实例数据（与 C++ 侧 BillboardInstanceData 布局一致）
// ============================================================================
struct BillboardInstanceData
{
    float3 Position;
    float Width;
    float Height;
    uint Mode; // BillboardMode 枚举值
    uint TextureArrayIndex;
    uint MaterialIndex;
    float PadS;
};

StructuredBuffer<BillboardInstanceData> gInstanceData : register(t12, space1);

// ============================================================================
// 顶点着色器输入/输出
// ============================================================================
// 无需顶点输入——所有数据从 StructuredBuffer 读取，VS 仅由 SV_VertexID/SV_InstanceID 驱动
struct VSOutput
{
    float3 CenterW : TEXCOORD0;
    float2 Size : TEXCOORD1;
    uint Mode : TEXCOORD2;
    uint TexIndex : TEXCOORD3;
    uint InstanceID : TEXCOORD4;
};

VSOutput VS(uint vertexID : SV_VertexID, uint instanceID : SV_InstanceID)
{
    VSOutput output;
    BillboardInstanceData inst = gInstanceData[instanceID];

    output.CenterW = inst.Position;
    output.Size = float2(inst.Width, inst.Height);
    output.Mode = inst.Mode;
    output.TexIndex = inst.TextureArrayIndex;
    output.InstanceID = instanceID;

    return output;
}

// ============================================================================
// 几何着色器输入/输出
// ============================================================================
struct GSOutput
{
    float4 PosH : SV_POSITION;
    float3 WorldPos : POSITION;
    float3 WorldNormal : NORMAL;
    float2 TexCoord : TEXCOORD;
    uint TexIndex : TEXCOORD1;
    uint InstanceID : TEXCOORD2;
    nointerpolation uint Mode : TEXCOORD3;
};

[maxvertexcount(4)] void GS(point VSOutput input[1], inout TriangleStream<GSOutput> triStream)
{
    float3 center = input[0].CenterW;
    float3 look = normalize(gCameraPos - center);

    float3 right, up;

    if (input[0].Mode == 0)
    { // AxisY 模式
        look.y = 0;
        look = normalize(look);
        right = normalize(cross(float3(0, 1, 0), look));
        up = cross(look, right);
    }
    else if (input[0].Mode == 1)
    { // Full 模式
        float3 upRef = float3(0, 1, 0);
        right = normalize(cross(upRef, look));
        up = cross(look, right);
    }
    else
    { // Spherical 模式
        float3 upRef = normalize(center);
        right = normalize(cross(upRef, look));
        up = cross(look, right);
    }

    float halfW = input[0].Size.x * 0.5f;
    float halfH = input[0].Size.y * 0.5f;

    float3 corners[4] = {
        center - right * halfW - up * halfH, // 左下
        center + right * halfW - up * halfH, // 右下
        center - right * halfW + up * halfH, // 左上
        center + right * halfW + up * halfH  // 右上
    };

    float2 texcoords[4] = {
        float2(0, 1), // 左下
        float2(1, 1), // 右下
        float2(0, 0), // 左上
        float2(1, 0)  // 右上
    };

    // 计算每个顶点的世界法线（面向相机，即 look 方向）
    float3 normal = normalize(gCameraPos - center);

    for (int i = 0; i < 4; ++i)
    {
        GSOutput output;
        output.WorldPos = corners[i];
        output.PosH = mul(float4(corners[i], 1.0f), gViewProj);
        output.WorldNormal = normal;
        output.TexCoord = texcoords[i];
        output.TexIndex = input[0].TexIndex;
        output.InstanceID = input[0].InstanceID;
        output.Mode = input[0].Mode;
        triStream.Append(output);
    }
}

// ============================================================================
// 像素着色器
// ============================================================================
float4 PS(GSOutput pin) : SV_Target
{
    // 使用无界纹理数组采样
    float4 texColor = gSharedTextures[pin.TexIndex].Sample(gSamplerLinearWrap, pin.TexCoord);

    // Alpha 裁剪：丢弃几乎透明的像素（与龙书一致，阈值 0.1f）
    clip(texColor.a - 0.1f);

    // 重新归一化插值后的法线
    float3 N = normalize(pin.WorldNormal);

    // 视线方向
    float3 V = normalize(gCameraPos - pin.WorldPos);

    // 从材质数组读取材质（与 color.hlsl 一致）
    uint matIndex = gInstanceData[pin.InstanceID].MaterialIndex;
    MaterialData matData = gMaterialData[matIndex];

    float3 albedo = matData.BaseColor.rgb * texColor.rgb;
    float metallic = matData.Metallic;
    float roughness = matData.Roughness;
    float ao = matData.Ambient;
    float3 emissive = matData.Emissive.rgb * matData.Emissive.w;

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

    // 直接光照（完整 PBR，与 color.hlsl 一致，逐光源遍历避免 ComputeLighting 的数组大小不匹配）
    float3 directLight = 0;
    for (uint i = 0; i < gNumDirLights; ++i)
        directLight += ComputeDirectionalLight(gLights[i], mat, N, V);
    for (uint j = gNumDirLights; j < gNumDirLights + gNumPointLights; ++j)
        directLight += ComputePointLight(gLights[j], mat, pin.WorldPos, N, V);
    for (uint k = gNumDirLights + gNumPointLights; k < gNumDirLights + gNumPointLights + gNumSpotLights; ++k)
        directLight += ComputeSpotLight(gLights[k], mat, pin.WorldPos, N, V);

    float3 litColor = ambient + directLight + emissive;

    return float4(litColor, texColor.a);
}