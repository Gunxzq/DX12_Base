//=============================================================================
// DrawNormals.hlsl — 绘制视空间法线到法线 RT
// 输入：标准场景几何体（Position + Normal），通过 InstanceData 获得 World 矩阵
// 输出：视空间法线（R16G16B16A16_FLOAT）
//=============================================================================

cbuffer cbPass : register(b0)
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
    float3 gPad;
};

struct InstanceData
{
    row_major float4x4 World;
    row_major float4x4 WorldInvTranspose;
    uint MaterialIndex;
    uint ReceiveShadow;
    uint ProbeIndex;
    float pad;
};

StructuredBuffer<InstanceData> gInstanceData : register(t0, space1);

struct VertexIn
{
    float3 PosL : POSITION;
    float3 NormalL : NORMAL;
    float2 TexC : TEXCOORD;
};

struct VertexOut
{
    float4 PosH : SV_POSITION;
    float3 NormalV : NORMAL;
};

VertexOut VS(VertexIn vin, uint instanceID : SV_InstanceID)
{
    VertexOut vout;
    InstanceData inst = gInstanceData[instanceID];
    float4 worldPos = mul(float4(vin.PosL, 1.0f), inst.World);
    vout.PosH = mul(worldPos, gViewProj);
    float3 worldNormal = normalize(mul(vin.NormalL, (float3x3)inst.WorldInvTranspose));
    vout.NormalV = normalize(mul((float3x3)gView, worldNormal));
    return vout;
}

float4 PS(VertexOut pin) : SV_Target
{
    float3 normalV = normalize(pin.NormalV);
    return float4(normalV, 0.0f);
}
