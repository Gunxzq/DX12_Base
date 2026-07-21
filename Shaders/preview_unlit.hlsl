// ========================================================================
// preview_unlit.hlsl — 资产预览 Unlit 着色器（纹理预览用）
//
// 无光照计算，直接采样纹理输出。用于纹理文件（.dds/.png）的预览，
// 避免 PBR 光照干扰纹理本身的视觉判断。
// ========================================================================

struct VSInput {
    float3 position : POSITION;
    float2 texcoord : TEXCOORD;
};

struct VSOutput {
    float4 position : SV_POSITION;
    float2 texcoord : TEXCOORD;
};

// ========================================================================
// 常量缓冲（64 bytes，对齐 D3D12 要求）
// ========================================================================

cbuffer PreviewCB : register(b0) {
    row_major float4x4 gWorldViewProj;  // 64 bytes
    // 注：与 PBR 版本的 PreviewCB 共用根签名，但 Unlit 只用到前 64 bytes
    // 后续字段存在但不会被读取
    float4 gDummy[12];
}

// ========================================================================
// 资源绑定
// ========================================================================

Texture2D gPreviewTexture : register(t0);
SamplerState gPreviewSampler : register(s0);

// ========================================================================
// 顶点着色器
// ========================================================================

VSOutput VS(VSInput input) {
    VSOutput output;
    output.position = mul(float4(input.position, 1.0f), gWorldViewProj);
    output.texcoord = input.texcoord;
    return output;
}

// ========================================================================
// 像素着色器 — 直接采样输出，无光照
// ========================================================================

float4 PS(VSOutput input) : SV_TARGET {
    float4 texColor = gPreviewTexture.Sample(gPreviewSampler, input.texcoord);
    return texColor;
}