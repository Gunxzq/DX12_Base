# 着色器与材质系统

> 本章分析 `data/` 目录下的 4 个 DX9 HLSL Effect 文件，以及从 .x 材质到 DX12 的映射方案。

---

## 一、着色器文件总览

所有文件均为 **DirectX 9 HLSL Effect (.fx)** 语法，用 **VS (Vertex Shader)** 和 **PS (Pixel Shader)** 组织，编译目标为 `vs_1_1` / `vs_2_0` / `ps_2_0`。

| 文件 | 技术 (Technique) | vs | ps | 功能 |
|------|-----------------|----|----|------|
| `Basic.txt` | Basic | vs_1_1 | ps_2_0 | 标准漫反射+高光+雾 |
| | SkinBasic | vs_2_0 | ps_2_0 | 骨骼蒙皮版本 |
| `Bloom.txt` | SpecluarOnly | vs_2_0 | ps_2_0 | HDR 高光提取 |
| | Bloom | — | ps_2_0 | 3-tap 模糊 |
| `Reflection.txt` | Reflection | vs_2_0 | ps_2_0 | 立方体贴图环境反射 |
| | Skin | vs_2_0 | ps_2_0 | 骨骼蒙皮+反射 |
| `ShadowMap.txt` | ShadowMap | vs_2_0 | ps_2_0 | 深度写入（光源视角） |
| | ShadowMapAlphaTest | vs_2_0 | ps_2_0 | 深度写入+AlphaTest |
| | ShadowMapSkin | vs_2_0 | ps_2_0 | 骨骼蒙皮深度写入 |
| | ShadowMapScene | vs_2_0 | ps_2_0 | 主渲染+4-tap PCF |

---

## 二、Basic.txt — 基础渲染

### 2.1 全局参数

```hlsl
float4x4 mW;        // 世界矩阵
float4x4 mP;        // 投影矩阵
float4x4 mWV;       // 世界×视图矩阵
float4x4 mWVP;      // 世界×视图×投影矩阵

float4 Diffuse;     // 漫反射颜色（光×材质）
float4 Specular;    // 高光颜色
float4 Ambient;     // 环境光颜色
float Power;        // 高光指数

texture MeshTex;    // 纹理
float3 LightDir;    // 光方向
float3 Eyepos;      // 视点位置
float2 vFog;        // (Far/(Far-Near), -1/(Far-Near))

int NoTexFlag = 0;  // 无纹理模式标记
```

### 2.2 标准 VS (vs_1_1)

```
输入: POSITION, NORMAL, TEXCOORD0
输出: Pos(投影空间), Col(漫反射), Spe(高光), MeshUV, Fog

漫反射: Ambient + Diffuse * max(0, N·L)
高光:   Specular * pow(max(0, R·E), Power)
雾:     vFog.x + Pos.w * vFog.y
```

### 2.3 标准 PS (ps_2_0)

```
PS: 返回 tex2D(MeshSmp, UV) * Col + Spe
NoTexPS: 返回 Col + Spe
```

### 2.4 骨骼蒙皮 VS (vs_2_0)

```hlsl
输入: POSITION, BLENDWEIGHT(float4), BLENDINDICES(float4),
      NORMAL, TEXCOORD0, NumBones(uniform)

// 关键：D3DCOLORtoUBYTE4 解码 BLENDINDICES
int4 IndexVector = D3DCOLORtoUBYTE4(BlendIndices);

// 循环 NumBones-1 次：加权累积位置和法线
// 最后剩余权重 = 1.0 - 已用权重之和
// 变换矩阵: mWorldMatrixArray[MAX_MATRICES=26]
```

- **MAX_MATRICES = 26**：最多支持 26 个骨骼
- **蒙皮方式**：线性混合蒙皮（Linear Blend Skinning）
- **顶点格式匹配**：`BLENDINDICES` 在 CPU 侧以 `R8G8B8A8_UINT` 存储，通过 `D3DCOLORtoUBYTE4` 将 float4 解码为 uint4

### 2.5 双技术选择

```hlsl
PixelShader PSArray[2] = { compile ps_2_0 PS(), compile ps_2_0 NoTexPS() };
technique Basic {
    pass P0 {
        VertexShader = compile vs_1_1 VS();
        PixelShader = (PSArray[NoTexFlag]);
    }
}

VertexShader SkinVSArray[4] = { compile vs_2_0 SkinVS(1..4) };
technique SkinBasic {
    pass P0 {
        VertexShader = (SkinVSArray[CurNumBones]);  // 运行时选择权重数
        PixelShader = compile ps_2_0 SkinPS();
    }
}
```

---

## 三、Bloom.txt — Bloom 特效

### 3.1 额外参数

```hlsl
texture InputTex;            // 输入图像
texture BloomTex;            // Bloom 中间缓冲
float2 SampleOffset[3];      // 采样偏移
```

### 3.2 流程

```
Pass 1 — SpecluarOnly:
  VS: 标准世界→投影变换
  PS: 仅输出高光颜色 (Specular * pow(N·H, Power))
  
Pass 2 — Bloom:
  VS: 全屏 Quad（无 VS = 默认）
  PS: 3-tap 采样 + 平均模糊 (SampleOffset[0..2])
```

> 注释掉的代码显示原设计是 15-tap 加权模糊，简化实现为 3-tap 等权平均。

### 3.3 与 Config.ini 的关联

```ini
HDR=1    ← Bloom/HDR 开关
```

---

## 四、Reflection.txt — 环境映射

### 4.1 额外参数

```hlsl
texture EnvTex;          // 立方体贴图
float Reflection;        // 反射强度 (0~1)
```

Sampler 类型：`samplerCUBE`（非 `sampler`）

### 4.2 反射计算

```hlsl
// 视线反射向量
float3 Eye = normalize(Eyepos - mul(pos, mW));
float3 r = reflect(-Eye, w_normal);  // 或手动: -Eye + 2*dot(N,Eye)*N

// PS 混合
float4 TexCol = tex2D(MeshSmp, In.MeshUV);
return float4(
    (Reflection * texCUBE(EnvSmp, EnvUV).xyz + (1-Reflection) * TexCol.xyz) * Col.xyz,
    TexCol.a * Col.a
) + Spe;
```

### 4.3 骨骼蒙皮版本 (Skin)

在骨骼蒙皮 VS 基础上，增加了 `EnvUV` 输出通道，PS 使用 `texCUBE` 采样。

---

## 五、ShadowMap.txt — 阴影映射

### 5.1 额外参数

```hlsl
texture ShadowTex;           // 阴影深度贴图（单通道）
int SHADOW_TEX_SIZE;         // 阴影贴图尺寸
float Margin;                // 深度偏移（Shadow Acne 消除）
float ShadowTexel;           // 1 / SHADOW_TEX_SIZE
float4x4 mLightWVP;          // 光源世界×视图×投影矩阵
float4x4 mLightWVPB;         // 光源矩阵（用于深度比较）
float4x4 mLightVP;           // 光源视图×投影矩阵
float ShadowPow = 0.3;       // 阴影强度（0=全黑, 1=无阴影）
```

### 5.2 渲染管线

```
Pass 1 — ShadowMap 写入（光源视角）
  CullMode = CW (背面剔除反转，解决 Shadow Acne)
  VS: 光源投影变换 (mLightWVP)
  PS: 输出 Depth.x / Depth.w (深度值)
  骨骼版本: mLightVP 变换 +蒙皮

Pass 2 — ShadowMapScene 主渲染
  VS: 标准变换 + 多点光源投影 (mLightWVP, mLightWVPB)
  PS: 4-tap PCF 阴影比较
```

### 5.3 4-tap PCF 算法

```hlsl
// 将顶点变换到光源投影空间
float2 uv = In.Depth.xy / In.Depth.w;      // [-1, 1]
uv = uv * 0.5 + 0.5;                         // [0, 1]
uv.y = 1 - uv.y;                             // D3D 坐标翻转

float z = In.Depth.z / In.Depth.w - Margin; // 当前深度-偏移

// 4-tap 百分比邻近滤波
t0 = (tex2D(ShadowSmp, uv)                .r < z) ? ShadowPow : 1;
t1 = (tex2D(ShadowSmp, uv + float2(Texel, 0)).r < z) ? ShadowPow : 1;
t2 = (tex2D(ShadowSmp, uv + float2(0, Texel)).r < z) ? ShadowPow : 1;
t3 = (tex2D(ShadowSmp, uv + float2(Texel, Texel)).r < z) ? ShadowPow : 1;

// 双线性插值
float2 f = frac(uv * SHADOW_TEX_SIZE);
float light_power = lerp(lerp(t0, t1, f.x), lerp(t2, t3, f.x), f.y);
color.rgb *= light_power;
```

### 5.4 与 Config.ini 的关联

```ini
Shadow=2    ← 阴影质量（0=关闭, 1=低, 2=高）
```

---

## 六、材质映射方案

### 6.1 .x 材质到 DX12 的映射

.x 文件中的 `Material` 块包含：

| 字段 | HLSL 类型 | 对应 DX12 |
|------|-----------|-----------|
| `faceColor` | ColorRGBA | 漫反射颜色 (Root Constant 或 CBV) |
| `power` | float | 高光指数 (Root Constant) |
| `specularColor` | ColorRGB | 高光颜色 (Root Constant) |
| `emissiveColor` | ColorRGB | 自发光颜色 (Root Constant) |
| `TextureFilename` | string | DDS 纹理路径 (SRV) |

### 6.2 材质 Pass 选择规则

| 原游戏技术 | 对应 Pass 类型 | 条件 |
|-----------|---------------|------|
| Basic | `Opaque` | 无环境贴图 |
| Reflection | `Reflection` | 有环境贴图 |
| ShadowMap | `ShadowDepth` | 阴影 Pass |
| ShadowMapScene | `ShadowReceiving` | 接收阴影的主渲染 |
| SpecluarOnly | `BloomSpecular` | HDR Bloom 高光提取 |
| Bloom | `BloomBlur` | 后期模糊 Pass |

### 6.3 从着色器推导的渲染状态

| 状态 | Basic | Reflection | ShadowMap | ShadowMapScene |
|------|-------|-----------|-----------|----------------|
| CullMode | CCW（默认） | CCW | **CW** | CCW（默认） |
| Lighting | 启用 | 启用 | **禁用** | 启用 |
| FogEnable | 启用 | 启用 | **禁用** | 启用 |
| AlphaBlend | 无 | 无 | 无 | 无 |
| ZEnable | 默认 | 默认 | 默认 | 默认 |

---

## 七、DX12 迁移要点

1. **Shader Model**: `vs_1_1`/`ps_2_0` → DX12 `SM 5_0`/`SM 6_0`（功能等价重写）
2. **Effect Framework**: `.fx` 运行时编译 → **离线编译 .hlsl** + PSO 缓存
3. **固定管线状态**: `.fx` pass 中的状态声明 → **显式配置 D3D12_GRAPHICS_PIPELINE_STATE_DESC**
4. **Sampler**: `sampler_state` 块 → **D3D12_STATIC_SAMPLER_DESC** 或堆
5. **纹理绑定**: `texture`/`sampler` 分离 → **Descriptor Heap + SRV**
6. **雾**: 不可编程雾阶段 → **自定义 PS 实现**
7. **蒙皮**: `D3DCOLORtoUBYTE4` → **CPU 侧预解码为 uint4**，或用 `R8G8B8A8_UINT` 格式直接读取
8. **阴影 PCF**: 硬件的 `D3DFMT_D16_UNORM` → DX12 `DXGI_FORMAT_D16_UNORM` + 手动 PCF
