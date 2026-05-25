// =================================================================================================
// cbPerObject: 每物体常量缓冲（每物体每帧变化）
// =================================================================================================
cbuffer cbPerObject : register(b0)
{
    row_major float4x4 gWorld;         // ← 添加 row_major
    row_major float4x4 gWorldInvTrans; // ← 添加 row_major
};

// =================================================================================================
// cbPass: 每帧常量缓冲（相机、光照等，每帧更新一次）
// =================================================================================================
cbuffer cbPass : register(b1)
{
    row_major float4x4 gView;     // ← 添加 row_major
    row_major float4x4 gProj;     // ← 添加 row_major
    row_major float4x4 gViewProj; // ← 添加 row_major
    row_major float4x4 gInvView;
    row_major float4x4 gInvProj;
    row_major float4x4 gInvViewProj;
    float3 gCameraPos;
    float gTotalTime;
    float gDeltaTime;
    float gNearPlane;
    float gFarPlane;
    float gAspectRatio;
    uint gFrameCount;
    uint gLightCount;
    float gAmbientIntensity;
    float gPad[3];
};

struct VertexIn
{
    float3 PosL : POSITION;
    float4 Color : COLOR;
};

struct VertexOut
{
    float4 PosH : SV_POSITION;
    float4 Color : COLOR;
};

VertexOut VS(VertexIn vin)
{
    VertexOut vout;

    // 列主序矩阵 × 列向量
    float4 worldPos = mul(float4(vin.PosL, 1.0f), gWorld);

    // 变换到裁剪空间
    vout.PosH = mul(worldPos, gViewProj);

    vout.Color = vin.Color;

    return vout;
}

float4 PS(VertexOut pin) : SV_Target
{
    return pin.Color;
}