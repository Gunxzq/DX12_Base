# BugFix: RenderDoc 无法捕获帧（无界纹理表 + 条件绑定）

**日期**: 2026-06-27  
**文件**: `Shaders/Common_PBR.hlsl`, `Shaders/color.hlsl`, `Runtime/Game/Scene/GameWorld.cpp`, `Engine/Renderer/Pipeline/OpaqueRenderer.cpp`  
**影响**: RenderDoc 无法捕获任何帧，但运行时正常渲染

---

## 问题现象

- 运行时正常渲染，无崩溃、无闪烁
- RenderDoc 无法捕获任何帧（点击捕获按钮后无响应或立即失败）
- 龙书例程可正常捕获

## 根因分析

引入无界纹理数组 `Texture2D gTextureMaps[] : register(t0, space2)` 后，`textureHeapStart` 从堆槽位 0 开始绑定。**`UINT_MAX` 无界表覆盖了整个 `CBV_SRV_UAV` 堆**，堆中夹杂了非纹理描述符（如 slot 4 的 Material Buffer StructuredBuffer），RenderDoc 在序列化描述符时检测到类型不匹配，拒绝捕获。

### 触发条件

1. `gTextureMaps[]` 的描述符表基址（`textureHeapStart`）指向堆起始（slot 0），而非纹理区域的真实起始
2. `UINT_MAX` 范围使 RenderDoc 试图序列化整个堆，包括非纹理描述符
3. 部分 slot（环境贴图 slot 8、探针 Cubemap Array slot 7）条件性绑定，未绑定时着色器仍声明了对应寄存器 → RenderDoc 检测到未绑定的根参数
4. `color.hlsl` 移除了 `#define DISABLE_ENV_REFLECTION`，导致 `gEnvMap : register(t10)` 始终声明，但 slot 8 可能未绑定

---

## 修复方案

- 调整 `textureHeapStart` 的偏移，使其只覆盖纹理描述符区域，避开 Material Buffer 等非纹理描述符
- 确保所有根签名 slot 都有兜底绑定（空句柄时绑定一个有效的空白描述符）

---

## 影响范围

- `Shaders/Common_PBR.hlsl` — `gTextureMaps[]` 声明、`gEnvMap` 声明
- `Shaders/color.hlsl` — 移除了 `#define DISABLE_ENV_REFLECTION`
- `Runtime/Game/Scene/GameWorld.cpp` — `textureHeapStart` 绑定点
- `Engine/Renderer/Pipeline/OpaqueRenderer.cpp` — 根签名 slot 3 (texTable)

## 回归检查

- 修改描述符堆布局后需用 RenderDoc 捕获一帧确认
- 验证无界纹理数组和天空盒/探针采样同时工作时是否正常
