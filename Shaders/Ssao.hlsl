//=============================================================================
// Ssao.hlsl — 屏幕空间环境光遮蔽
// 全屏四边形，输入场景深度+法线，输出 AO 贴图
// 参考：Frank Luna, Chapter 21
//=============================================================================

cbuffer cbSsao : register(b0)
{
    row_major float4x4 gView;
    row_major float4x4 gProj;
    row_major float4x4 gInvProj;
    row_major float4x4 gProjTex;
    float4   gOffsetVectors[14];
    float    gOcclusionRadius;      // 采样半径
    float    gOcclusionFadeStart;   // 衰减起始距离
    float    gOcclusionFadeEnd;     // 衰减结束距离
    float    gSurfaceEpsilon;       // 表面容差
};

Texture2D gNormalMap    : register(t0);
Texture2D gDepthMap     : register(t1);
Texture2D gRandomVecMap : register(t2);

SamplerState gsamPointClamp  : register(s0);
SamplerState gsamLinearClamp : register(s1);
SamplerState gsamDepthMap    : register(s2);
SamplerState gsamLinearWrap  : register(s3);

static const int gSampleCount = 14;

static const float2 gTexCoords[6] =
{
    float2(0.0f, 1.0f),
    float2(0.0f, 0.0f),
    float2(1.0f, 0.0f),
    float2(0.0f, 1.0f),
    float2(1.0f, 0.0f),
    float2(1.0f, 1.0f)
};

struct VertexOut
{
    float4 PosH : SV_POSITION;
    float3 PosV : POSITION;
    float2 TexC : TEXCOORD0;
};

VertexOut VS(uint vid : SV_VertexID)
{
    VertexOut vout;
    vout.TexC = gTexCoords[vid];
    vout.PosH = float4(2.0f * vout.TexC.x - 1.0f, 1.0f - 2.0f * vout.TexC.y, 0.0f, 1.0f);

    // 将四边形角变换到视空间近平面
    float4 ph = mul(vout.PosH, gInvProj);
    vout.PosV = ph.xyz / ph.w;

    return vout;
}

// 遮挡函数：根据深度差决定遮挡量
float OcclusionFunction(float distZ)
{
    float occlusion = 0.0f;
    if (distZ > gSurfaceEpsilon)
    {
        float fadeLength = gOcclusionFadeEnd - gOcclusionFadeStart;
        occlusion = saturate((gOcclusionFadeEnd - distZ) / fadeLength);
    }
    return occlusion;
}

// NDC 深度 → 视空间深度
float NdcDepthToViewDepth(float z_ndc)
{
    // z_ndc = A + B/viewZ, gProj[2][2]=A, gProj[3][2]=B
    float viewZ = gProj[3][2] / (z_ndc - gProj[2][2]);
    return viewZ;
}

float4 PS(VertexOut pin) : SV_Target
{
    // p: 当前像素的视空间位置
    // n: 当前像素的视空间法线
    // q: 采样点位置
    // r: 实际遮挡点

    // 从 G-buffer 读取法线（编码 N*0.5+0.5，世界空间）
    float3 n_world = gNormalMap.SampleLevel(gsamPointClamp, pin.TexC, 0.0f).xyz * 2.0f - 1.0f;
    // 变换到视空间（SSAO 在视空间计算）
    float3 n = normalize(mul(n_world, (float3x3)gView));
    float pz = gDepthMap.SampleLevel(gsamDepthMap, pin.TexC, 0.0f).r;
    pz = NdcDepthToViewDepth(pz);

    // 重建完整的视空间位置
    float3 p = (pz / pin.PosV.z) * pin.PosV;

    // 提取随机向量，[0,1] → [-1, +1]，归一化确保 reflect 结果稳定
    float3 randVec = normalize(2.0f * gRandomVecMap.SampleLevel(gsamLinearWrap, 4.0f * pin.TexC, 0.0f).rgb - 1.0f);

    float occlusionSum = 0.0f;

    // 在法线半球内采样邻近点
    [unroll]
    for (int i = 0; i < gSampleCount; ++i)
    {
        // 反射偏移向量以获得随机均匀分布
        float3 offset = reflect(gOffsetVectors[i].xyz, randVec);

        // 如果偏移在法线背面则翻转
        float flip = sign(dot(offset, n));

        // 在遮挡半径内采样邻近点
        float3 q = p + flip * gOcclusionRadius * offset;

        // 投影 q 生成纹理坐标
        float4 projQ = mul(float4(q, 1.0f), gProjTex);
        projQ /= projQ.w;

        // 在深度图中查找 q 方向的最近深度
        float rz = gDepthMap.SampleLevel(gsamDepthMap, projQ.xy, 0.0f).r;
        rz = NdcDepthToViewDepth(rz);

        // 重建完整的遮挡点位置
        float3 r = (rz / q.z) * q;

        // 计算遮挡
        float distZ = p.z - r.z;
        float dp = max(dot(n, normalize(r - p)), 0.0f);
        float occlusion = dp * OcclusionFunction(distZ);

        occlusionSum += occlusion;
    }

    occlusionSum /= gSampleCount;

    float access = 1.0f - occlusionSum;

    // 增强对比度，使 AO 效果更明显
    return saturate(pow(access, 6.0f));
}
