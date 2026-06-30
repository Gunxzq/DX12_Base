//=============================================================================
// SsaoBlur.hlsl — SSAO 边缘保持高斯模糊
// 水平 pass 和垂直 pass 通过 gHorizontalBlur 区分
//=============================================================================

cbuffer cbRootConstants : register(b0)
{
    bool gHorizontalBlur;
    float2 gTexelSize;  // 1/width, 1/height（由 C++ 填充）
};

Texture2D gInputMap : register(t0);

SamplerState gsamPointClamp : register(s0);

static const float2 gTexCoords[6] =
{
    float2(0.0f, 1.0f),
    float2(0.0f, 0.0f),
    float2(1.0f, 0.0f),
    float2(0.0f, 1.0f),
    float2(1.0f, 0.0f),
    float2(1.0f, 1.0f)
};

// 高斯权重（sigma=2.5, 半径=5）
static const float gWeights[11] =
{
    0.0033f, 0.0121f, 0.0347f, 0.0775f, 0.1353f, 0.1848f,
    0.1975f, 0.1649f, 0.1076f, 0.0549f, 0.0219f
};

// 像素偏移（对应 texel 位置）
static const float gTexelOffsets[11] =
{
    -5.0f, -4.0f, -3.0f, -2.0f, -1.0f, 0.0f,
     1.0f,  2.0f,  3.0f,  4.0f,  5.0f
};

struct VertexOut
{
    float4 PosH : SV_POSITION;
    float2 TexC : TEXCOORD0;
};

VertexOut VS(uint vid : SV_VertexID)
{
    VertexOut vout;
    vout.TexC = gTexCoords[vid];
    vout.PosH = float4(2.0f * vout.TexC.x - 1.0f, 1.0f - 2.0f * vout.TexC.y, 0.0f, 1.0f);
    return vout;
}

float4 PS(VertexOut pin) : SV_Target
{
    float2 texelSize = gTexelSize;

    float4 color = 0.0f;

    [unroll]
    for (int i = 0; i < 11; ++i)
    {
        float2 offset = gHorizontalBlur
            ? float2(gTexelOffsets[i] * texelSize.x, 0.0f)
            : float2(0.0f, gTexelOffsets[i] * texelSize.y);

        color += gWeights[i] * gInputMap.SampleLevel(gsamPointClamp, pin.TexC + offset, 0.0f);
    }

    return color;
}
