# BugFix: Grid Shader Missing `row_major` Matrix Declaration

## 问题描述

`Shaders/grid.hlsl` 中 `gViewProj` 矩阵声明为 `float4x4`（默认 **column_major**），而项目中所有其他着色器（`lighting.hlsl`、`DrawNormals.hlsl`、`Common_PBR.hlsl` 等）均使用 `row_major float4x4`。

C++ 端的 `DirectX::XMMATRIX` 以**行主序**存储，HLSL 默认的 `float4x4` 是**列主序**。两者不匹配导致 `mul(float4(worldPos, 1.0f), gViewProj)` 计算出错误的裁剪坐标，网格被裁剪到视锥外，不可见。

## 修复

在 `grid.hlsl` 的 `gViewProj` 前加 `row_major` 关键字：

```hlsl
cbuffer GridCB : register(b0)
{
    row_major float4x4 gViewProj; // 64B
    float4 gGridParam;            // x=spacing, y=tilesPerSide, z=cameraSnapX, w=cameraSnapZ
};
```

## 约束规则

所有从 C++ 端传入的 `XMMATRIX` 矩阵常量，在 HLSL 中必须声明为 `row_major float4x4`，否则矩阵数据会被错误解释为列主序，导致变换结果错误。

## 注意

`float4` 不是矩阵类型，不能加 `row_major`/`column_major` 修饰，否则编译错误：
```
error X3077: non-matrix types cannot be declared 'row_major' or 'column_major'
```