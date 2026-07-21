//==============================================================================
// grid.hlsl — 3D 无限网格着色器（大 Quad + 像素着色器计算）
//
// 方案：渲染一个大 Quad，在 PS 中计算网格线位置
// 功能：
//   - 多级 LOD（主/次网格）
//   - 反走样线（smoothstep）
//   - 指数距离衰减
//   - 三轴高亮线（X=红, Y=绿, Z=蓝）
//   - Y 轴垂直网格线（原点处）
//==============================================================================

cbuffer GridCB : register(b0)
{
    row_major float4x4 gViewProj;   // 64B
    float4 gCameraPos;               // xyz = 相机世界位置, w = unused
    float4 gGridParams;              // x = minorSpacing, y = majorSpacing, z = lineWidth, w = fadeDist
    float4 gSnapOffset;              // xz = 相机 snap 偏移, w = 网格半边长
};

struct VS_OUT
{
    float4 pos : SV_POSITION;
    float3 worldPos : TEXCOORD0;
};

//==============================================================================
// 顶点着色器：大 Quad 顶点 → 世界空间 → 裁剪空间
//==============================================================================

VS_OUT VS(float3 localPos : POSITION)
{
    // localPos 是 [-1,1] 范围的单位 Quad
    // 扩展到网格覆盖范围，并偏移到相机 snap 位置
    float3 worldPos = float3(
        localPos.x * gSnapOffset.w + gSnapOffset.x,
        0.0f,
        localPos.z * gSnapOffset.w + gSnapOffset.z
    );

    VS_OUT vout;
    vout.worldPos = worldPos;
    vout.pos = mul(float4(worldPos, 1.0f), gViewProj);
    return vout;
}

//==============================================================================
// 像素着色器：计算网格线
//==============================================================================

float4 PS(VS_OUT pin) : SV_TARGET
{
    float3 pos = pin.worldPos;
    float2 posXZ = pos.xz;
    float dist = length(posXZ - gCameraPos.xz);

    // ── 距离衰减（指数） ──
    float fade = exp(-dist / gGridParams.w);
    fade = saturate(fade);

    // ── 次网格线（小间距） ──
    float2 minorGrid = abs(frac(posXZ / gGridParams.x + 0.5) - 0.5) * gGridParams.x;
    float minorLine = 1.0f - smoothstep(0.0f, gGridParams.z * 0.5f, min(minorGrid.x, minorGrid.y));
    float minorAlpha = minorLine * fade * 0.5f;

    // ── 主网格线（大间距） ──
    float2 majorGrid = abs(frac(posXZ / gGridParams.y + 0.5) - 0.5) * gGridParams.y;
    float majorLine = 1.0f - smoothstep(0.0f, gGridParams.z, min(majorGrid.x, majorGrid.y));
    float majorAlpha = majorLine * fade * 0.85f;

    // ── 三轴高亮线 ──
    float axisWidth = gGridParams.z * 6.0f;
    float3 axisColor = float3(0.0f, 0.0f, 0.0f);
    float axisAlpha = 0.0f;

    // X 轴（红色）：Z=0, Y=0 的线
    if (abs(pos.z) < axisWidth && abs(pos.y) < axisWidth)
    {
        axisColor = float3(1.0f, 0.0f, 0.0f);
        axisAlpha = fade;
    }

    // Z 轴（蓝色）：X=0, Y=0 的线
    if (abs(pos.x) < axisWidth && abs(pos.y) < axisWidth)
    {
        axisColor = float3(0.0f, 0.0f, 1.0f);
        axisAlpha = max(axisAlpha, fade);
    }

    // Y 轴（绿色）：X=0, Z=0 的垂直线
    if (abs(pos.x) < axisWidth && abs(pos.z) < axisWidth)
    {
        axisColor = float3(0.0f, 1.0f, 0.0f);
        axisAlpha = max(axisAlpha, fade);
    }

    // ── 合成 ──
    float3 gridColor = float3(0.5f, 0.7f, 1.0f); // 亮蓝色，自发光感
    float alpha = max(minorAlpha, majorAlpha);
    alpha = max(alpha, axisAlpha);

    // 轴颜色混合：只在轴线上方显示轴颜色
    float3 color = gridColor;
    if (axisAlpha > 0.0f)
    {
        color = lerp(gridColor, axisColor, axisAlpha / max(alpha, 0.001f));
    }

    return float4(color, alpha);
}