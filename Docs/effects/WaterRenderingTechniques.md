# 大型引擎水渲染技术调研

> 日期：2026-08-05
> 状态：📋 调研文档（记录大型引擎水面渲染机制，指导本项目水渲染进阶）
> 关联：`Docs/architecture/rendering/WaterSystemArchitecture.md`（本项目水系统架构）、
> `Docs/architecture/rendering/RenderPipelineSpecification.md`（§10.5 数据上传铁律）、
> `Docs/effects/ScreenSpaceDistortion.md`（特效目录既有文档）
> 背景：本项目水渲染已打通（程序化网格 + Water 材质槽 + FrameSync 上传 + Transparent 阶段），
> 待解决：水面高度越过地形（MPD `#WaterY=-0.5` 参数丢失）、岸线硬边、区域内波浪差异化。

---

## 1. 概述

大型引擎（Unreal / Unity HDRP / Godot）的水面渲染收敛到同一套核心思路：

```
水网格（可单块、可无限、可分区）
  ├── 顶点：Gerstner 波 / 多频带模拟（波形 = 世界位置的函数，天然支持顶点级差异）
  ├── 与地形交接：场景深度测试（隐藏地形下水面）+ 岸线深度渐隐（消除硬边）
  ├── 区域差异化：纹理贴图（Water Mask / WaterInfoTexture）控制网格内不同区块参数
  └── 光斑：Caustics 纹理投影（斜向投影贴水面）
```

**关键认知**：大型引擎**不会为不同波浪参数物理切网格块**，而是：
- 波形本身就是位置函数（Gerstner：`sin(pos.x * freq + time)`）→ "具体到某些顶点"天然成立
- 区域参数用 **纹理掩码** 控制（同一网格内不同区域不同波高/频率）

---

## 2. Unreal Engine 水系统

### 2.1 体系结构

| 组件 | 职责 |
|:--|:--|
| `WaterZone` | 水区域容器，定义 `ZoneExtent`（最大范围）、`RenderTargetResolution`（WaterInfoTexture 分辨率）、`CaptureZOffset`（渲染高度偏移） |
| `WaterMeshComponent` | **四叉树瓦片网格**：`TileSize`（LOD0 瓦片尺寸）+ 同心 LOD（近密远疏）+ `FarDistanceMesh`（远处低细分网格）——瓦片划分只为 **LOD 性能**，非视觉正确性 |
| `WaterBody*`（Ocean/Lake/River） | 水体 Actor，`FillWaterZoneWithOcean`（海洋填满整个 Zone） |
| `WaterInfoTexture` | **RenderTarget 纹理数组**（`TextureRenderTarget2DArray`），每帧渲染水深信息，材质采样用 |
| `WaterTerrainComponent` | 挂在地形 Actor 上，把地形深度渲染进 WaterInfoTexture |

### 2.2 WaterInfoTexture — 水深信息纹理

- WaterZone 从上方（`CaptureZOffset` 之上）渲染场景 → 得到世界空间覆盖的水深纹理
- 含水深信息：`waterDepth = 水面高度 - 地形高度`（半精度 16bit / 全精度 32bit 可选）
- 材质采样 → 岸线渐隐、泡沫强度、水深着色

### 2.3 Gerstner 波生成器

```cpp
// GerstnerWaterWaves.h — 波形生成器基类
virtual float GetWaveHeightAtPosition(const FVector& InPosition, float InWaterDepth,
                                      float InTime, FVector& OutNormal) const;
virtual float GetWaveAttenuationFactor(const FVector& InPosition, float InWaterDepth,
                                       float InTargetWaveMaskDepth) const;
```

- `UGerstnerWaterWaveGeneratorSimple`：NumWaves / MinMaxAmplitude / MinMaxWavelength / 风角 / 陡度 / 随机种子
- `UGerstnerWaterWaveGeneratorSpectrum`：海洋学波谱，octave 八度（波长 2 倍递增）
- **波形是位置的函数** → 逐顶点查询波高/法线
- `GetWaveAttenuationFactor`：**水深衰减**（浅水区波高衰减，防止波浪越过地形/岸线）
- `RecomputeWaves()`：运行时改参后重新计算缓存

### 2.4 岸线处理（DepthFade）

- 材质中用 `Distance To Nearest Surface`（距离场）或 **场景深度比较**控制岸线
- `waterDepth = 水面深度 - 场景深度` → 透明度/泡沫随水深渐隐
- 论坛实践：DepthFade 需要斜度补偿（`dot(cameraVec, vertexNormal)` 修正），浅水坡度足够时效果自然

---

## 3. Unity HDRP 水系统

### 3.1 几何类型（Geometry Type）

| 类型 | 说明 |
|:--|:--|
| `Quad` | 方形水面 |
| `Instanced Quads` | 多实例网格拼接，保持高顶点密度（有限水面） |
| `Custom Mesh` | 自定义网格（顶点 Y 保持水平） |
| `Infinite` | **海洋专用：无限水面**，由 Global Volume 界定边界——单块无限网格 + 深度测试，无需手动切块 |

### 3.2 多频带模拟（Multi-band Simulation）

- **Ocean/Sea/Lake**：3 个频带 = 2 个 Swell（涌浪，低频远距）+ 1 个 Ripples（涟漪，高频近距）
- **River**：2 个频带 = Agitation（等效 Swell）+ Ripples
- 输入：Distant Wind（涌浪）+ Local Wind（涟漪）+ Current（洋流，方向独立于风）
- 频带分离 → 大水面与小水面都自然

### 3.3 Water Mask 纹理 — 区域差异化核心

> **关键机制（与"纹理贴图做水区分块"直接对应）**：

```
Water Mask 纹理：
  R 通道 → 衰减 Swell 频带 1
  G 通道 → 衰减 Swell 频带 2
  B 通道 → 衰减 Ripples 频带
```

- **一张纹理控制网格内任意区域的波高**——正是"把水的分块画在纹理上"的官方实现
- 用世界空间坐标采样（UV 节点 = worldPos）
- 同一水面网格内，不同区域可以完全不同的波浪表现（如港湾平静、外海汹涌）

### 3.4 其他能力

- **吸收距离（Absorption Distance）**：水下可视距离
- **焦散（Caustics）**：斜向投影的焦散纹理（Virtual Plane Distance / Caustics Plane Blend）
- **Patch / Grid** 概念：Patch = 模拟区域尺寸，Grid = 渲染几何（恒矩形）
- **CPU 模拟**：脚本可查询波高（浮力等游戏逻辑用）

---

## 4. 其他参考

### 4.1 Godot

- 深度纹理采样做岸线，大平面 + 相机跟随网格

### 4.2 Crest Water（Unity 付费资产）

- Wave Spline 技术：沿样条控制河流/湖泊/岸线区域波
- 同时支持 FFT 频谱 + Gerstner 波
- 动态涟漪模拟（物体入水波）

---

## 5. 核心机制对比表

| 维度 | Unreal | Unity HDRP | 本项目现状 |
|:--|:--|:--|:--|
| 水面网格 | 四叉树瓦片 + 同心 LOD | Infinite / Instanced Quads / Quad | 程序化 grid 单块（4×420×420） |
| 与地形交接 | 深度测试 + DepthFade + 距离场 | 深度测试 + 透明折射队列 | 深度测试已开（LESS + 不写深度）✅ |
| 岸线渐隐 | WaterInfoTexture 水深 | Fade / 吸收距离 | ❌ 未实现 |
| 区域差异化 | WaterInfoTexture（世界空间） | **Water Mask 纹理（R/G/B 频带）** | ❌ 未实现 |
| 波形 | Gerstner（位置函数 + 水深衰减） | 多频带模拟（Swell+Ripples） | 简单 sin 叠加（water.hlsl VS） |
| 焦散光斑 | 有 | Caustics 纹理 | ❌ 未实现 |
| 参数运行时改 | RecomputeWaves() | CPU 模拟查询 | WaterManager 每帧上传 |

---

## 6. 关键结论与启示

### 6.1 "纹理贴图做水区分块"是标准做法 ✅

用户提出的思路与大型引擎一致：
- **UE**：`WaterInfoTexture`（世界空间水深纹理）天然编码了水的区域范围
- **Unity**：`Water Mask` 纹理（R/G/B 通道控制不同频带波高）——**官方支持"一张纹理分区控制水面"**

**结论**：4 个 WaterBlock 合并成一块大水面后，用纹理采样做事实上的区域差异化，完全可行且是主流做法。物理切块只保留用于变换/剔除/参数归属，视觉上应无缝合并。

### 6.2 水面高度 vs 地形（本项目当前问题）

- MPD 原始参数 `#WaterY = -0.5`（水面基准高度）在转换时**丢失**（`DxSceneWaterBlock` 无该字段，`convert_waterblocks.js` 硬编码 y=0）
- 水面 y=0 高于地面（mapChip y≈-1.998）→ 水"浮"在地面上方
- **最小修复**：WaterBlock `transform.position.y = -0.5`（对齐 MPD `#WaterY`），山体/建筑自然穿出

### 6.3 深度检测的正确性

"大面积水面覆盖在地形之下，通过深度检测隐藏部分"——**正确且标准**：
- WaterRenderer PSO 已配置 `DepthEnable=TRUE + DepthFunc=LESS + DepthWriteMask=ZERO` ✅
- 地形（Opaque 阶段写深度）高于水面的像素 → 水被剔除
- 只需水面基准高度低于地形最高点，山体/建筑自然穿出

---

## 7. 落地路径（本项目）

| 阶段 | 内容 | 对齐引擎 | 说明 |
|:--:|:--|:--|:--|
| **P1** | WaterBlock `y: 0 → -0.5`（MPD `#WaterY`）+ 转换端保留 baseY | MPD 原始数据 | 纯 JSON + 转换器字段，先解决水面越过地面 |
| **P2** | 岸线深度渐隐：water.hlsl 采样场景深度 SRV → `shoreFade = saturate(waterDepth / fadeRange)` | UE DepthFade | **Editor 端已落地（2026-08-05，见 §7.1）**；消除水面与地形硬切边 |
| **P3** | 区域差异化：WaterInfo 纹理（世界空间水深/区域参数） | UE WaterInfoTexture / Unity Water Mask | 大水面内不同区块不同浪；`WaterConstants` 纹理索引已预留 |
| **P4** | 焦散光斑：Caustics 纹理斜向投影 | Unity Caustics | 你提到的"光斑" |

### 引擎已预留的接线位

```
WaterConstants（FrameResourceTypes.h:127-128）：
  uint32_t DepthTextureIndex;      // 深度纹理索引（P2 用）
  uint32_t RefractionTextureIndex; // 折射纹理索引
  uint32_t NormalTextureIndex;     // 法线纹理索引
water.hlsl cbWater 已声明对应字段（gDepthTextureIndex 等）
```

### 7.1 P2 岸线深度渐隐落地记录（2026-08-05）

**改造清单（Editor 端已实施，Game 端暂缓）**：

| 文件 | 改动 |
|:--|:--|
| `Engine/Renderer/Pipeline/WaterRenderer.h/.cpp` | 根签名新增 **slot 7: t11,space0 场景深度 SRV**；`BeginFrame` 新增 `depthSRV` 参数（默认空句柄，不绑定则降级）；绑定 `SetGraphicsRootDescriptorTable(7, depthSRV)` |
| `Shaders/water.hlsl` | 声明 `Texture2D gSceneDepth : register(t11)`；`cbWater.gPad1` 复用为 `gFadeRange`；PS 岸线渐隐：`GetDimensions` 自取深度尺寸 → 采样场景深度 → `waterDepth = sceneDepth - pin.PosH.z` → `shoreFade = saturate(waterDepth / gFadeRange)` → `alpha *= shoreFade`（`gFadeRange <= 0` 时禁用，降级纯色水） |
| `Engine/Renderer/WindowFrameResources` | **深度 SRV 已存在**（`GetDepthSRV()`，D32→R32_FLOAT，无需新增）——Editor 端直接复用 |
| `Editor/EditorLib/Scene/EditorViewport.h` | 新增 `GetDepthSRV()` 转发方法（转发 `m_windowResources->GetDepthSRV()`） |
| `Editor/EditorLib/Core/Editor.cpp` | EditorWaterRenderSystem `BeginFrame` 传 `m_viewport->GetDepthSRV()`（深度 SRV，岸线渐隐） |

**两端深度对比（一致性前提）**：

| 维度 | Editor 端 | Game 端 |
|:--|:--|:--|
| 深度缓冲来源 | `EditorViewport::GetDepthResource()` → **WindowFrameResources 离屏 RT 池**（非交换链） | `GetSwapChainManager().GetDepthStencilBuffer()`（**交换链深度**） |
| 深度 SRV | `WindowFrameResources::GetDepthSRV()`（已实现） | `DeviceContext->GetDepthSRV()`（AO 先例） |
| 水渲染时深度状态 | COMMON → DEPTH_READ → COMMON（对称屏障 ✅） | 未接（暂缓） |
| 内容 | 同一场景 Opaque 阶段写入的深度，**内容一致但资源不同** | 同上 |

**⚠️ 一致性要点**：两端必须**各自绑定各自深度缓冲的 SRV**（Editor 用离屏 WindowFrameResources 深度，Game 用交换链深度），不能跨端复用对方资源；且采样时深度须处于 `DEPTH_READ`（或 PIXEL_SHADER_RESOURCE）状态。

**Game 端暂缓**：`GameRenderPipeline.cpp` WaterRenderSystem 未绑定 depthSRV（`BeginFrame` 不传该参数，着色器 `gFadeRange=0` 自动降级），后续接入时参照 Editor 端：传 `GetDepthSRV()` + 补 `DEPTH_WRITE → DEPTH_READ → DEPTH_WRITE` 对称屏障。

---

## 8. 纹理贴图枚举（全部使用可能性）

> 深度缓冲只解决"水的范围"（哪里被地形遮挡），**不解决"水的差异"**（不同区域波浪/外观不同）。
> 差异由各类纹理贴图驱动——大型引擎按用途枚举如下。

### 8.1 分类总表

| 类别 | 贴图/输入 | 引擎 | 用途 |
|:--|:--|:--|:--|
| **范围/区域** | Water Mask 纹理（R/G/B 通道） | Unity | R/G 衰减 Swell 两频带、B 衰减 Ripples——**区域差异化核心** |
| | WaterInfoTexture（RenderTarget 数组） | UE | 世界空间水深信息（含水深/区域掩码） |
| | 场景深度缓冲 | UE/Unity | 岸线范围、水面遮挡（DepthFade） |
| | 高度上下限贴图（RGBA 存 min/max） | 自研常见 | **一贴图区分区块范围**：R=波高下限 G=波高上限 B=频率 A=方向/掩码 |
| **表面细节** | Normal Map（多层 panner） | UE/Unity | 波纹法线（小波反向平铺产生涟漪） |
| | Flow / Perturbation Map | 常见 | 流向/扰乱，UV 偏移驱动 |
| | Bump Offset 视差纹理 | UE | 伪深度凹凸（焦散/闪光用） |
| | Height Map | UE | World Position Offset 驱动波浪起伏 |
| **特效** | Caustics 焦散纹理 | UE/Unity | 光斑投影（投影器/光函数/模拟缓冲） |
| | Foam 泡沫纹理 + Foam Mask | UE/Unity | 波峰/岸线泡沫（R 通道掩码衰减） |
| | Sparkles 表面闪光 | UE | 水面高光闪烁 |
| | Refraction 折射（IOR 参数/纹理） | UE/Unity | 水下扭曲（水 IOR≈1.33） |
| **体积/着色** | Scattering / Absorption 系数 | UE/Unity | 水体散射/吸收（体积参数，非贴图） |
| | 深浅水色（Dark/Light color） | UE | 菲涅尔驱动的深浅水色过渡 |
| **输入数据** | Current Map（R/G 方向 + B 影响度） | Unity | 洋流方向/强度（独立于风） |
| | Simulation Buffers | Unity | GetCausticsBuffer / GetFoamBuffer / GetSimulationMaskBuffer 运行时查询 |

### 8.2 关键机制详解

**A. 高度上下限贴图（用户关注点）**

RGBA 打包一贴图区分区块范围——同一水面网格内不同区域不同波浪：

```hlsl
// 世界坐标采样（UV = worldPos.xy / regionSize）
float4 region = gWaterRegionMap.Sample(gSamplerLinearWrap, worldPos.xz * gRegionScale);
float ampScale    = region.r;      // 波高下限（0~1 倍率）
float ampUpper    = region.g;      // 波高上限（0~1 倍率）
float freqScale   = region.b;      // 频率倍率
float dirMask     = region.a;      // 方向掩码/区域边界
// VS 位移：amplitude = lerp(ampLower, ampUpper, noise) * baseAmplitude
float y = GetWaveHeight(posL, freqScale * gWaveFrequency, time) * ampScale;
```

对齐：Unity Water Mask（RGB 三频带）、UE WaterInfoTexture（世界空间区域数据）。

**B. 扰乱法线（Perturbation Normal）**

多层 Normal Map 反向 panner + Flow Map 驱动 UV 扰动：

```hlsl
// 双层法线反向平铺（UE Community Wiki 经典做法）
float2 uvA = worldUV * gNormalTiling + float2(gTime * 0.03, -gTime * 0.02);
float2 uvB = worldUV * gNormalTiling + float2(-gTime * 0.1, gTime * 0.1);
float3 nA = gNormalMapA.Sample(gSamplerLinearWrap, uvA).xyz * 2 - 1;
float3 nB = gNormalMapB.Sample(gSamplerLinearWrap, uvB).xyz * 2 - 1;
float3 N = normalize(float3(nA.xy + nB.xy, 1.0)); // 合成法线
```

**C. 光斑焦散（Caustics）**

- Unity：模拟缓冲 + 虚拟平面投影（Virtual Plane Distance / Tiling / Intensity）
- UE：投影器（Decal Projector）或 Light Function 投影焦散纹理到水底

### 8.3 本项目当前纹理使用 vs 潜力

| 贴图 | 本项目现状 | 潜力 |
|:--|:--|:--|
| baseColor（sea.dds） | ✅ 已用 | — |
| 场景深度 | ✅ 已绑定（P2 岸线渐隐，`GetDepthSRV`） | — |
| Normal Map | ❌ 无（`NormalTextureIndex` 已预留） | 波纹法线 |
| Water Mask / 区域贴图 | ❌ 无 | 区块差异化（P3） |
| Caustics | ❌ 无 | 光斑（P4） |
| Foam | ❌ 无 | 波峰/岸线泡沫 |
| Refraction | ❌ 无（`RefractionTextureIndex` 已预留） | 水下扭曲 |

### 8.4 贴图生成工具链（CrazyBump）

> CrazyBump 从**单张源图**（Diffuse/基准图）分析生成多张辅助贴图：
> **Normal Map（切线空间，核心）、Specular Map、Height Map、Occlusion Map、Bump Map**。
> 与手绘/PS 滤镜相比，对深度重建（法线）效果更优，但对噪声表面和强明暗对比容易失真。

**针对水的辅助贴图清单（对齐 WaterConstants 预留索引）**：

| 贴图 | CrazyBump 生成 | 用途 | 引擎接线 |
|:--|:--|:--|:--|
| Normal Map | ✅ 核心能力 | 水面微皱褶/涟漪细节（叠加在 VS 程序化波浪上） | `NormalTextureIndex` 已预留 |
| Specular Map | ✅ | 水面高光/反射强度分布（湿滑高光） | 材质槽 specular |
| Height Map | ✅ | 视差偏移（Parallax Mapping）/ 波浪高度增强 | 材质槽 height |
| Occlusion Map | ✅ | 波纹沟壑遮蔽（water1_OCC 已有） | 材质槽 ao |
| Bump Map | ✅（并入 Normal） | 低精度凹凸 | — |

**⚠️ 平铺 UV 注意事项（生成前必须满足）**：

1. **源图必须无缝（seamless）**——CrazyBump 官方要求输入纹理可无缝平铺，否则生成的法线/高度贴图在平铺边界出现接缝
2. **水的 UV 应使用世界坐标**（`worldPos.xz * tilingScale`，对齐 Unity `UV = worldPos`）——当前 `vin.TexCoord` 是**局部 UV**，4 块水网格各自从 0 平铺，纹理在块边界断裂（波形已世界坐标连续，纹理未同步）。世界坐标 UV 让纹理跨块连续，与波形一致
3. **法线贴图是切线空间**——需 VS 输出完整 TBN（含 Bitangent）到 PS 变换，且双层法线反向 panner（§8.2B）动画在平铺边界依赖 WRAP 采样
4. **水面是两套独立细节**：VS 程序化波浪（世界坐标，宏观流动）+ 贴图法线（切线空间，微观细节）——互补叠加，不冲突

---

## 9. 参考资料

- Unreal: Water Meshing System / WaterZone / WaterMeshComponent / GerstnerWaterWaves（dev.epicgames.com）
- Unity HDRP: Water System 文档（Geometry Type / Water Mask / Multi-band Simulation / Caustics）
- Crest Water 4（Unity Asset Store）
- UE 论坛: shoreline depth fade 实践（Distance To Nearest Surface + DepthFade 斜度补偿）
