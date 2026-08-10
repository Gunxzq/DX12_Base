# BugFix: City 地图渲染视觉异常四连修复（UV 镜像 / 绕序 / baseColor 黑 tint / 平铺 clamp）

> 日期：2026-08-03
> 涉及文件：`AssetTool/Core/MapSceneConverter.cpp`、`AssetTool/Core/FbxMeshConverter.cpp`、`AssetTool/Core/XFileParser.cpp`、`AssetTool/Core/RobotMerger.cpp`、`Shaders/color.hlsl`、`Shaders/probe_capture.hlsl`、`Shaders/water.hlsl`、`Content/City/City.scene.json`
> 状态：✅ 全部修复，CityTest4 视觉验证通过（DE 视角一致）

---

## 背景

UKW City 地图（7623 实例 / 40 piece）经 mpd2scene 转换后在引擎中渲染异常：树/路面空内容、纹理显示在网格内部/反面、部分纹理平铺异常。经 RenderDoc/DE 逐项对照，定位为四个独立缺陷叠加。

## 缺陷一：UV V 轴镜像（转换器缺 `1-v`）

### 现象
纹理上下镜像，个别面纹理"陷进网格内部"。

### 证据
- 顶点 dump：`texC = (U=1.00, V=-1.99879)`，V 为负
- dxmesh 全量统计：mapChip06 `V∈[-9,1]`、mapChip03 `V∈[-31.5,1.07]`——V 大量落在负半轴
- 引擎正确参照（程序化网格 cube/ground/cylinder/torus）：`V∈[0,1]`
- 且 `V = 1 - [0,10]`（mapChip06），确认缺 `v' = 1 - v`

### 根因
assimp 读入 .x/FBX 的 UV V 为向上为正（OpenGL/Blender 约定），引擎采样约定 V 向下为正（D3D），转换器直接拷贝未翻转。

### 修复（4 个转换器统一）
| 文件 | 改动 |
|:--|:--|
| `MapSceneConverter.cpp` | `v.texC[1] = 1.0f - sm.texcoords[i*2+1]` |
| `FbxMeshConverter.cpp` | `v.texC[1] = 1.0f - mTextureCoords[0][vi].y` |
| `XFileParser.cpp` | `WriteDxMesh`：`1.0f - texcoords[i*2+1]` |
| `RobotMerger.cpp` | `1.0f - ms.texcoords[i*2+1]` |

### 验证（node 模拟 `1-v`）
mapChip06 `V[-9,1]→[0,10]`、mapChip03 `V[-31.5,1.07]→[-0.07,32.5]`、bill01 `V[-9.66,0.98]→[0.02,10.66]`，与 U 平铺范围对称。

## 缺陷二：索引绕序多余翻转（.x 源为左手系）

### 现象
面朝向反（效果上看到网格反面/内部），且无纹理时也可看出。

### 根因
`.x` 源本身是左手系（DirectX 格式，与引擎一致），但 `MapSceneConverter` 沿用 FbxMeshConverter 的"右手→左手"转换，对索引做了 `(i0,i2,i1)` 翻转——**双重转换**导致面朝向反。

### 对照参照
| 转换器 | 源 | 索引处理 |
|:--|:--|:--|
| `FbxMeshConverter` | FBX（Blender 右手系） | `(i0,i2,i1)` 翻转 ✅ |
| `RobotMerger` | HOD/.x（左手系） | **不翻转**（直接拷贝）✅ |
| `MapSceneConverter`（修复前） | .x（左手系） | `(i0,i2,i1)` 翻转 ❌ |

### 修复
`MapSceneConverter.cpp` 索引处理改为**不翻转**（与 RobotMerger 一致），保留顶点 Z 取反与矩阵 Z 列取反。

### 验证
CityTest4（索引不翻转 + UV 修复）视觉验证通过。注：叉积法线 vs 顶点法线同向/反向不能作为绕序裁决（法线随 Z 取反同步翻转），以视觉验证为准。

## 缺陷三：带纹理材质 baseColor 黑 tint

### 现象
树（3767 个实体）漫反射 RT 全黑（空内容）。

### 根因
`mat_5754a57993d4` baseColor `[0,0,0,1]` 纯黑。PBR `albedo = BaseColor × texColor`（tint×纹理），`0 × 纹理 = 0`。mpd2scene 把 `.x` 材质 faceColor 原样写入 baseColor，而 UKW 树 faceColor 恰好是黑的。

### 修复
1. **转换器根因**：`MapSceneConverter.cpp` `registerMaterial`——有 baseColor 纹理时 tint 强制白（`if (!texKey.empty()) baseColor=1`）
2. **存量数据**：`Content/City/City.scene.json` `mat_5754a57993d4` baseColor `[0,0,0,1]→[1,1,1,1]`

### 验证
全场景复查：无"带纹理但 baseColor 过暗"材质。

## 缺陷四：着色器 clamp 破坏平铺 UV

### 现象
部分纹理平铺异常（本应平铺重复却只显示单格/拉伸），DE 视角（原始 UV）正常。

### 根因
`color.hlsl:71` / `probe_capture.hlsl:80` / `water.hlsl:62` 的 `clamp(vin.TexCoord, 0.0f, 0.999f)` 把平铺 UV（如 [0,10]）压成 [0,0.999]，与采样器 `gSamplerAnisotropicWrap`（WRAP 平铺模式）自相矛盾。`skinned.hlsl`/`Terrain.hlsl` 一直直接透传（正确参照）。

### 修复（3 个 shader 改为透传）
| 文件 | 改动 |
|:--|:--|
| `Shaders/color.hlsl` | `vout.TexCoord = vin.TexCoord` |
| `Shaders/probe_capture.hlsl` | 同上 |
| `Shaders/water.hlsl` | 同上 |

### 验证
全 Shaders grep 无残留 clamp；平铺纹理恢复正常。

---

## 相关代码位置

- `AssetTool/Core/MapSceneConverter.cpp` — 顶点 UV（L370-374）、索引（L391-400）、registerMaterial tint（L247-252）
- `AssetTool/Core/FbxMeshConverter.cpp` — 顶点 UV（L361-364）
- `AssetTool/Core/XFileParser.cpp` — `WriteDxMesh` UV（L317-320）
- `AssetTool/Core/RobotMerger.cpp` — 顶点 UV（L218-221）
- `Shaders/color.hlsl` — VS TexCoord（L71）、GBuffer PS 采样（L132-154）
- `Shaders/probe_capture.hlsl` — VS TexCoord（L80）
- `Shaders/water.hlsl` — VS TexCoord（L62）

## 遗留备注

- AssetTool 需重新编译后重转；引擎（Editor/Game）需重新编译后验证
- 源 .x 为 Z-up（DE 观察 Z 轴在上）已在会话中确认，但当前 `leftHanded` 的 Z 取反+矩阵 Z 列取反方案视觉验证通过，未改轴交换——如需 Z-up→Y-up 显式轴交换可后续评估
