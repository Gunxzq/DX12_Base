# BugFix: 光照 PASS 根签名缺少静态采样器导致 PSO 创建失败

- **发现日期**: 2026-07-02
- **模块**: `GameWorld::RegisterLightingPass` → `LightingRenderer`
- **现象**: 延迟光照 PASS 不执行，画面无光照输出。无编译错误提示，因为 PSO 创建失败但没有走标准 `ThrowIfFailed` 路径，仅通过 `OutputDebugString` 输出后 `return`，System 被跳过注册。

## 触发路径

```
GameWorld::Initialize()
  └→ RegisterLightingPass()
       ├→ D3DCompileFromFile("lighting.hlsl") → 编译成功
       ├→ D3D12SerializeRootSignature(...)
       │   └→ rsDesc.Init(7 params, 0 samplers, ...)  ← NumStaticSamplers=0
       ├→ device->CreateRootSignature(...)            → 成功（根签名不验证采样器）
       └→ device->CreateGraphicsPipelineState(...)    → 失败！
            └→ 根签名未声明 sampler s3，但 lighting.hlsl PS 使用了
                SamplerState gSamplerPointClamp : register(s3)
```

## 根因

`RegisterLightingPass` 内联创建根签名时没有定义任何静态采样器（`NumStaticSamplers = 0`），但 `lighting.hlsl` 的 PS 声明了 `SamplerState gSamplerPointClamp : register(s3)` 并用于采样 G-buffer 纹理。D3D12 要求根签名必须描述所有着色器使用的资源（包括采样器），缺少 `s3` 导致 `CreateGraphicsPipelineState` 返回失败。

由于代码使用 `if (FAILED(...)) { OutputDebugString; return; }` 而非 `ThrowIfFailed`，失败后函数直接 return，System 从未被注册到 SystemRegistry，光照 PASS 永远不执行，且用户看不到标准异常报告。

## 修复

1. 将内联实现重构为独立 `LightingRenderer` 类（继承 `IRenderer`），着色器编译使用标准错误输出 + `throw std::runtime_error`
2. 根签名添加 `CD3DX12_STATIC_SAMPLER_DESC` 定义 `s3 PointClamp` 采样器
3. 新类的 `LoadShaders()` 包含完整的 `OutputDebugStringA` 错误日志

## 关联问题

所有全屏 Quad 渲染（SSAO、Blur、光照 Pass）均需注意：
- 绕序为逆时针 → 需设置 `CullMode = NONE`
- 新命令列表无默认视口 → 每次 Draw 前需调用 `RSSetViewports` + `RSSetScissorRects`
