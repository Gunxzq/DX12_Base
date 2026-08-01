// ========================================================================
// line.hlsl — 调试线框渲染（WireframeManager）
//
// 拓扑：LINELIST
//   VS：直传 Position + Color
//   GS：按屏幕空间线宽把线段展开为四边形（2 三角形）——D3D12 原生 LINELIST
//       无可靠线宽，GS 展开是大型引擎（Unity/UE）的标准做法
//   PS：直出顶点色（半透明，SRC_ALPHA 混合）
//
// CB 布局（b0，与 WireframeManager::LineCBData 对应）：
//   gViewProj      float4x4   视口投影矩阵
//   gParams.x/y   屏幕宽/高（像素）
//   gParams.z     线宽（像素）
// ========================================================================

cbuffer LineCB : register(b0) {
    row_major float4x4 gViewProj;
    float4 gParams; // x=screenWidth, y=screenHeight, z=lineWidth, w=pad
};

struct VSInput {
    float3 position : POSITION;
    float4 color : COLOR;
};

struct GSInput {
    float3 position : POSITION;
    float4 color : COLOR;
};

struct PSInput {
    float4 position : SV_POSITION;
    float4 color : COLOR;
};

// ========================================================================
// VS — 直传
// ========================================================================
GSInput VS(VSInput input) {
    GSInput output;
    output.position = input.position;
    output.color = input.color;
    return output;
}

// ========================================================================
// GS — 线段按屏幕空间线宽展开为四边形
//
// 思路：把两个端点投影到 clip 空间，再换算到屏幕像素坐标，沿线段垂直方向
// 偏移 ±lineWidth/2 像素生成 4 个角点，输出 2 个三角形。
// 远平面 w<=0 的线段直接丢弃（调试线框近裁剪即可，避免投影翻转伪影）。
// ========================================================================

// 屏幕像素坐标 → NDC（Y 翻转：屏幕 Y 向下 → NDC Y 向上）
// 注：HLSL 不支持函数体内嵌套函数定义，故定义为文件级函数
float2 ScreenToNDC(float2 s) {
    s.y = gParams.y - s.y;
    return s / gParams.xy * 2.0f - 1.0f;
}

[maxvertexcount(4)]
void GS(line GSInput input[2], inout TriangleStream<PSInput> stream) {
    // ── 投影端点到 clip 空间 ──
    float4 p0 = mul(float4(input[0].position, 1.0f), gViewProj);
    float4 p1 = mul(float4(input[1].position, 1.0f), gViewProj);

    // 近平面裁剪：任一端点 w <= 0（在观察相机后方或近平面内）则丢弃整条线段
    // 这是 3D 线框几何体方案的关键——不再出现屏幕空间投影的"双 Z 交叉"伪影
    if (p0.w <= 0.0f || p1.w <= 0.0f)
        return;

    // ── clip → NDC → 屏幕像素（Y 向下，与视口一致） ──
    float2 ndc0 = p0.xy / p0.w;
    float2 ndc1 = p1.xy / p1.w;
    float2 screen0 = (ndc0 * 0.5f + 0.5f) * gParams.xy;
    float2 screen1 = (ndc1 * 0.5f + 0.5f) * gParams.xy;
    screen0.y = gParams.y - screen0.y; // NDC Y 向上 → 屏幕 Y 向下
    screen1.y = gParams.y - screen1.y;

    // ── 线段方向 + 垂直方向（屏幕空间） ──
    float2 dir = screen1 - screen0;
    float len = length(dir);
    if (len < 1e-4f)
        return;
    float2 normal = float2(-dir.y, dir.x) / len; // 垂直单位向量

    // ── 距离自适应线宽 ──
    // 视锥体等调试线框在远处投影变小后固定像素线宽不可观察，按线段到相机深度
    // （clip w，透视投影下 = view-space 深度）动态放大展开宽度，保持屏幕可观察性：
    //   distScale = clamp(最近端深度 / 参考距离, 1, 4)
    // 用 min(p0.w, p1.w)（线段最近端）而非平均深度：视锥体各线段（汇聚线/近远矩形）
    // 都以近端深度为基准，同一视锥体内粗细统一，避免汇聚线（近→远深度差大）avgDepth 偏大导致过粗。
    // 正交投影 clip w = 1，distScale = 1/referenceDistance < 1 → clamp 到 1，即保持固定线宽。
    //
    // 注意：不对展开宽度做基于线段屏幕长度（len）的压缩——那是"粗细不一致"的根因：
    // 不同屏幕投影长度的线段（近矩形长/远矩形短）会被 len*0.5 压缩成不同宽度。
    // 展开宽度只由统一 distScale 决定，所有线段粗细一致。
    float nearDepth = min(p0.w, p1.w);
    float distScale = clamp(nearDepth / gParams.w, 1.0f, 4.0f);
    float2 halfW = normal * (gParams.z * 0.5f) * distScale;

    // ── 4 个屏幕角点 ──
    float2 c0 = screen0 - halfW;
    float2 c1 = screen0 + halfW;
    float2 c2 = screen1 - halfW;
    float2 c3 = screen1 + halfW;

    // ── 屏幕 → NDC → clip（保留原 w 以保持深度正确） ──
    // 注意：输出的是 clip 空间坐标，必须 NDC 乘以 w 还原（光栅化会再次除以 w），
    // 否则顶点位置会被 1/w 缩放（近处线爆炸、远处线塌缩）
    float4 v0 = float4(ScreenToNDC(c0) * p0.w, p0.z, p0.w);
    float4 v1 = float4(ScreenToNDC(c1) * p0.w, p0.z, p0.w);
    float4 v2 = float4(ScreenToNDC(c2) * p1.w, p1.z, p1.w);
    float4 v3 = float4(ScreenToNDC(c3) * p1.w, p1.z, p1.w);

    // ── 输出 2 个三角形（逆时针绕序，CullMode=NONE 无需担心） ──
    PSInput outVert;
    outVert.color = input[0].color;
    outVert.position = v0;
    stream.Append(outVert);
    outVert.position = v1;
    stream.Append(outVert);
    outVert.position = v2;
    stream.Append(outVert);
    stream.RestartStrip();

    outVert.color = input[1].color;
    outVert.position = v1;
    stream.Append(outVert);
    outVert.position = v3;
    stream.Append(outVert);
    outVert.position = v2;
    stream.Append(outVert);
    stream.RestartStrip();
}

// ========================================================================
// PS — 直出顶点色
// ========================================================================
float4 PS(PSInput input) : SV_Target {
    return input.color;
}
