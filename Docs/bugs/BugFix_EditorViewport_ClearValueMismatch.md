# BugFix: EditorViewport 颜色 RT 优化 Clear Value 与实际 Clear 颜色不匹配

## 问题描述

D3D12 Debug Layer 输出警告：

```
ID3D12CommandList::ClearRenderTargetView: The clear values do not match
those passed to resource creation. The clear operation is typically slower
as a result; but will still clear to the desired value.
```

`EditorViewport.cpp` 中颜色 RT 资源创建时指定的优化 Clear Value 为 `{0.0f, 0.0f, 0.0f, 0.0f}`，但 `ClearRenderTargetView` 实际清除颜色为 `{0.1f, 0.1f, 0.2f, 1.0f}`（深蓝灰色背景）。两者不一致导致 GPU 无法使用 fast-clear 路径，降级为慢速清除。

## 修复

`EditorViewport.cpp` L96–101，将优化 Clear Value 与实际清除颜色对齐：

```cpp
// 修改前
D3D12_CLEAR_VALUE colorClear = {};
colorClear.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
colorClear.Color[0] = 0.0f;
colorClear.Color[1] = 0.0f;
colorClear.Color[2] = 0.0f;
colorClear.Color[3] = 0.0f;

// 修改后
D3D12_CLEAR_VALUE colorClear = {};
colorClear.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
colorClear.Color[0] = 0.1f;
colorClear.Color[1] = 0.1f;
colorClear.Color[2] = 0.2f;
colorClear.Color[3] = 1.0f;
```

## 影响范围

仅 `EditorViewport.cpp` 一处，无其他 RT 存在此问题。

## 约束规则

创建 `D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET` 资源时若传入 `D3D12_CLEAR_VALUE`，其颜色值必须与后续 `ClearRenderTargetView` 调用一致，否则 D3D12 无法启用 fast-clear 路径，产生性能警告。