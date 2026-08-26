

Texture2D<float> gDepthSrc : register(t0); // 深度图 SRV（全分辨率，i=0 拷贝源）
RWTexture2D<float> gSrc : register(u1);    // HZB 上一级 mip UAV（i>=1 源）
RWTexture2D<float> gDst : register(u0);    // HZB 当前 mip UAV（i 目标）

cbuffer HzbBuildCB : register(b0)
{
    uint2 gSrcSize; // 源 mip 尺寸（宽, 高）
    uint gSrcMip;   // 源 mip 层级（0 = 深度图；>=1 = HZB mip）
    uint gDstMip;   // 目标 mip 层级（UAV 已按该 mip slice 绑定）
};

// 源采样：gSrcMip==0 读深度 SRV，否则读 HZB UAV（分支在 dispatch 间一致）
float LoadSrc(uint2 coord)
{
    if (gSrcMip == 0)
        return gDepthSrc.Load(int3(coord, 0));
    return gSrc[coord];
}

[numthreads(8, 8, 1)] void CSMain(uint3 dispatchThreadID : SV_DispatchThreadID)
{
    uint2 dstCoord = dispatchThreadID.xy;

    // 拷贝模式（i==0，mip0 = 深度图 1:1）：目标尺寸 = 源尺寸，每线程直拷 1 像素
    if (gDstMip == 0)
    {
        if (dstCoord.x >= gSrcSize.x || dstCoord.y >= gSrcSize.y)
            return;
        gDst[dstCoord] = LoadSrc(dstCoord); // gSrcMip==0 → 读深度图
        return;
    }

    // 降采样模式（i>=1）：目标尺寸 = max(1, 源尺寸/2)（D3D mip 链 floor 语义）；越界线程返回
    uint2 dstSize = max(1u, gSrcSize >> 1);
    if (dstCoord.x >= dstSize.x || dstCoord.y >= dstSize.y)
        return;

    // 源 2×2 采样坐标（clamp 到源 mip 内——非 2 的幂尺寸边缘）
    uint2 srcBase = dstCoord << 1;
    uint2 srcMax = gSrcSize - 1u;

    float d00 = LoadSrc(uint2(srcBase.x, srcBase.y));
    float d10 = LoadSrc(uint2(min(srcBase.x + 1u, srcMax.x), srcBase.y));
    float d01 = LoadSrc(uint2(srcBase.x, min(srcBase.y + 1u, srcMax.y)));
    float d11 = LoadSrc(uint2(min(srcBase.x + 1u, srcMax.x), min(srcBase.y + 1u, srcMax.y)));

    // max 归约（最远面）——遮挡剔除保守方向
    gDst[dstCoord] = max(max(d00, d10), max(d01, d11));
}
