//==============================================================================
// Shadow.hlsl - 阴影贴图着色器
//
// 功能：
//   - 方向光阴影 (DirShadowVS / ShadowPS)       : 正交 VP 矩阵
//   - 点光源阴影 (PointShadowVS / PointShadowGS / ShadowPS) : 立方体 6 面
//   - 聚光灯阴影 (SpotShadowVS / ShadowPS)       : 透视 VP 矩阵
//
// 输入布局：仅需要 Position
//==============================================================================

//==============================================================================
// 物体常量缓冲 (b0)
//==============================================================================
cbuffer cbShadowObject : register(b0)
{
    row_major float4x4 gWorld;
}

//==============================================================================
// 方向光阴影常量 (b1)
//==============================================================================
cbuffer cbDirShadow : register(b1)
{
    row_major float4x4 gDirLightViewProj;
    float gDirShadowMapSize;
    float gDirBias;
    float gDirNormalBias;
    float gDirShadowStrength;
    uint gDirShadowMapIndex;
    float gDirPad[3];
}

//==============================================================================
// 点光源阴影常量 (b1, 替换使用)
//==============================================================================
cbuffer cbPointShadow : register(b1)
{
    row_major float4x4 gPointLightViewProj[6];
    float3 gPointLightPosition;
    float gPointShadowMapSize;
    float gPointBias;
    float gPointNormalBias;
    float gPointShadowStrength;
    float gPointRange;
    uint gPointShadowMapIndex;
    float gPointPad[2];
}

//==============================================================================
// 聚光灯阴影常量 (b1, 替换使用)
//==============================================================================
cbuffer cbSpotShadow : register(b1)
{
    row_major float4x4 gSpotLightViewProj;
    float gSpotShadowMapSize;
    float gSpotBias;
    float gSpotNormalBias;
    float gSpotShadowStrength;
    float gSpotPower;
    uint gSpotShadowMapIndex;
    float gSpotPad[2];
}

//==============================================================================
// 顶点输入/输出
//==============================================================================
struct VertexIn
{
    float3 PosL : POSITION;
};

struct VertexOut
{
    float4 PosH : SV_POSITION;
};

struct GeoOut
{
    float4 PosH : SV_POSITION;
    uint RTIndex : SV_RenderTargetArrayIndex;
};

//==============================================================================
// 方向光阴影 VS — 使用方向光 VP 矩阵
//==============================================================================

// InstanceData 结构体（与 C++ 侧 FrameResourceTypes.h 保持一致）
// C++ 布局: XMFLOAT4X4 World (64B) + XMFLOAT4X4 WorldInvTranspose (64B)
//          + uint32 MaterialIndex + uint32 ReceiveShadow + float Pad[2]
//          = 144 bytes (含 HLSL 对齐填充)
struct InstanceData
{
    row_major float4x4 World;             // 64 bytes
    row_major float4x4 WorldInvTranspose; // 64 bytes
    uint MaterialIndex;                   // 4 bytes
    uint ReceiveShadow;                   // 4 bytes
};

#ifdef USE_INSTANCING
// Instanced 模式：从 StructuredBuffer 读取每实例 World 矩阵
StructuredBuffer<InstanceData> gInstanceData : register(t12, space1);

VertexOut DirShadowVS(VertexIn vin, uint instanceID : SV_InstanceID)
{
    VertexOut vout;

    float4 worldPos = mul(float4(vin.PosL, 1.0f), gInstanceData[instanceID].World);
    vout.PosH = mul(worldPos, gDirLightViewProj);

    return vout;
}
#else
// Standard 模式：使用 gWorld CBV
VertexOut DirShadowVS(VertexIn vin)
{
    VertexOut vout;

    float4 worldPos = mul(float4(vin.PosL, 1.0f), gWorld);
    vout.PosH = mul(worldPos, gDirLightViewProj);

    return vout;
}
#endif

//==============================================================================
// 点光源阴影 VS — 只传递世界坐标给 GS
//==============================================================================
VertexOut PointShadowVS(VertexIn vin)
{
    VertexOut vout;

    float4 worldPos = mul(float4(vin.PosL, 1.0f), gWorld);
    vout.PosH = worldPos; // GS 中完成 6 面 VP 变换

    return vout;
}

//==============================================================================
// 点光源阴影 GS — 将三角形输出到立方体贴图 6 个面
//==============================================================================
[maxvertexcount(18)] void PointShadowGS(triangle VertexOut input[3], inout TriangleStream<GeoOut> triStream)
{
    for (int face = 0; face < 6; ++face)
    {
        GeoOut output;
        output.RTIndex = face;

        for (int v = 0; v < 3; ++v)
        {
            output.PosH = mul(input[v].PosH, gPointLightViewProj[face]);
            triStream.Append(output);
        }
        triStream.RestartStrip();
    }
}

//==============================================================================
// 聚光灯阴影 VS — 使用聚光灯 VP 矩阵
//==============================================================================
VertexOut SpotShadowVS(VertexIn vin)
{
    VertexOut vout;

    float4 worldPos = mul(float4(vin.PosL, 1.0f), gWorld);
    vout.PosH = mul(worldPos, gSpotLightViewProj);

    return vout;
}

//==============================================================================
// 阴影 PS (方向光/聚光灯) — 空函数，深度由 GPU 自动写入
//==============================================================================
void ShadowPS(VertexOut pin)
{
    // 不处理透明物体，无需 Alpha Test
    // 深度由 GPU 自动写入 DepthStencil
}

//==============================================================================
// 阴影 PS (点光源，GS 输出 GeoOut 含 SV_RenderTargetArrayIndex)
//==============================================================================
void ShadowPS_Point(GeoOut pin)
{
    // 同 ShadowPS，深度自动写入
}
