# 能力评估 — 引擎管线 × UKW 资产格式适配分析

> 评估日期: 2026-07-09
> 评估范围: 当前 DX12 引擎渲染管线能力 vs UKW PowerUp Kit 原始资产格式
> 目标: 确认"现代化展示"的最小可行路径

---

## 一、引擎已具备的能力

### 渲染管线（显著优于原版 DX9）

| 能力 | 技术方案 | 备注 |
|:-----|:---------|:------|
| **延迟渲染** | 4-Gbuffer (Albedo/Normal/Material/WorldPos) | R8G8B8A8 + R16G16B16A16 ×2 + R8G8B8A8 |
| **PBR 光照** | GGX 微表面模型，Fresnel-Schlick | 支持 256 盏灯（方向/点/聚光） |
| **阴影** | 方向光/点光源/聚光灯，9-tap PCF | ShadowMap 深度 Pass + 软阴影 |
| **SSAO** | 屏幕空间环境光遮蔽 | AO 效果提升 |
| **天空盒** | Cubemap 环境反射 | 随相机移动 |
| **反射探针** | Cubemap Array，最多 64 个 | 动态反射捕获 |
| **水面** | Wave Simulation + 反射/折射/泡沫 | 多频正弦波叠加 |
| **公告板** | GS 扩展 BillboardRenderer | — |
| **蒙皮** | 4-weight Linear Blend Skinning | StructuredBuffer<float4x4> 骨骼矩阵 |
| **蒙皮顶点格式** | Position+Normal+Tangent+TexC+BoneWeights+BoneIndices | 支持 R8G8B8A8_UINT 骨骼索引 |

### 资产加载管线

| 能力 | 状态 | 说明 |
|:-----|:-----|:------|
| **异步加载** | ✅ | 三阶段 cpuWork→gpuWork→onComplete |
| **.dxmesh 网格** | ✅ | 自有二进制格式，44B(静态)/64B(蒙皮) 顶点 |
| **DDS 纹理** | ✅ | 异步加载到 GPU + SRV 分配 |
| **材质系统** | ✅ | PBR MaterialDesc (.mat JSON) + GPU MaterialBuffer |
| **骨架管理器** | ✅ | SkeletonManager / SkeletonData.h 已存在 |
| **ECS 框架** | ✅ | Entity + Component + System + Message + Registry |

---

## 二、格式差距分析

### 缺失模块（已实现 ✅）

| 模块 | 对应格式 | 输入 | 输出 | 状态 |
|:-----|:---------|:-----|:-----|:------|
| **XOR 解密** | .hod/.ani/.mpd/.sdt | 加密二进制 | 明文二进制 | ✅ 已实现 |
| **.x 解析器** | .x (xof 0303bin) | assimp + XOR 自动检测 | DxMeshHeader + MaterialDesc | ✅ 已实现（assimp）|
| **.hod 解析器** | .hod (HOD) | XOR 解密 → 逐 entry | 骨架树 + 部件父链 + 变换矩阵 | ✅ 已实现 |
| **.x → .dxmesh 转换** | .x → 自有格式 | 命令行 AssetTool x2mesh | .dxmesh 二进制 | ✅ 已实现 |
| **.hod → txt 输出** | .hod → 可读文本 | GUI/CLI 批量转换 | .hod.txt（A/B/Name/Matrix）| ✅ 已实现 |

### 仍缺失的模块

| 模块 | 对应格式 | 输入 | 输出 | 优先级 |
|:-----|:---------|:-----|:-----|:-------|
| **.spt(场景) 解析器** | Script.spt (Shift-JIS) | 文本指令 | 瓦片映射/建筑/光照/雾/天空 | ★ 中 |
| **.mpd 解析器** | .mpd (MPD) | XOR 解密后二进制 | 瓦片索引→.x 文件映射表 | ★ 中 |
| **.spt(机体) 解析器** | Script.spt (Shift-JIS) | 文本指令 | 角色属性/COLORSET/武器插槽 | ★ 中 |
| **.ani 解析器** | .ani (AN2+HD2) | XOR 解密后二进制 | 骨骼关键帧数据 | ☆ 低 |

### 数据流

```
UKW 原始文件                      引擎内部格式
─────────────────                 ────────────────
.x (xof 0303bin)  ──→ 解析 ──→  DxMeshHeader + 顶点/索引数据
                                  ↓
                             MeshLoadTask → GPU VB/IB

.hod (XOR)        ──→ 解密 ──→  骨架树 + 部件名 + 4×4 变换矩阵
                     ──→ 解析 ──→  ECS SkeletonComponent

.spt (场景)       ──→ 解析 ──→  SceneDefinition (瓦片/建筑/光照/雾)
.mpd (XOR)        ──→ 解密 ──→  TileIndexMap
                     ──→ 解析 ──→  Tile→.x 映射表

.dds/.png         ──→ 直接加载 → GPU Texture (DDS 解析器已有)
.tex.png          ──→ 直接加载 → GPU Texture

.spt (机体)       ──→ 解析 ──→  ActorDefinition (HP/AI/配色/武器)
.ani (XOR)        ──→ 解密 ──→  BoneKeyframe[] → AnimationClip
                     ──→ 解析
```

---

## 三、材质映射策略

### 3.1 机体 — 纯色材质（无纹理）

**现状**: KD-03 无 tex.png，所有部件仅靠 .x Material 的 faceColor/specularColor/power；
KD-04~08 的 tex.png (163KB) 是图集，但同样是简单色块。

**策略**: 不需要纹理贴图生成，直接映射材质参数

| .x 材质字段 | → PBR 映射 | 说明 |
|:------------|:-----------|:------|
| `faceColor` (ColorRGBA) | `MaterialParams.baseColor` | 漫反射基础色 |
| `specularColor` (ColorRGB) | `Metallic` + `Roughness` 推导 | 高光强→低粗糙度，高光色→金属度 |
| `power` (float) | `Roughness = 1.0 / (power + 1.0)` | 高光指数转粗糙度经验公式 |
| `COLORSET(r,g,b)` | Root Constant override BaseColor | 运行时换色，零开销 |
| `TextureFilename` | `BaseColorTextureIndex` | 若有 tex.png 则采样 |

**视觉效果提升来源**（按权重排列）：
1. PBR GGX 光照模型（vs Blinn-Phong）
2. 9-tap PCF 阴影（vs 4-tap）
3. SSAO 环境遮蔽（原版无）
4. 环境反射探针（原版无）
5. 雾效自定义实现（vs DX9 固定雾阶段）
6. Bloom（待追加）

### 3.2 地图 — 真实纹理

**现状**: map/City/ 等目录有大量 DDS 照片纹理（道路/墙面/建筑纹理）

**策略**: 可通过 CrazyBump 处理生成 PBR 贴图

`road.dds / wall.dds / BuildingsHigh.dds` → `CrazyBump` → 法线贴图 + 粗糙度贴图 + AO 贴图

这些是**真正可以利用 CrazyBump 的地方**——照片纹理的细节可以被提取为法线/粗糙度信息。

---

## 四、社区 MOD 资源

### 已确认的可用资源

| MOD | 更新时间 | 大小 | 内容 | 格式兼容性 |
|:----|:---------|:-----|:------|:-----------|
| **EXVS2XB Mod** (ModDB) | 2024-05 | 1.24GB + 56MB | Gundam Versus 整合，新机体/平衡/玩法 | ✅ 与原版完全一致 |
| **United MOD Beta2.0** (游侠网) | 2024-11 | 1.8GB | 整合创作，新机体/地图/武器/宇宙图 | ✅ 与原版完全一致 |
| **高清重置机体包 V2.1** | 2025-2026 | 未明确 | 所有原版机体贴图升级到 4K | ✅ 仅替换 tex.png |
| **SeedMod** | 持续 | 不定 | 高达 SEED 主题 MOD | ⚠️ 不同 XOR key |

**核心结论**: 所有 MOD 的资产格式（.x/.hod/.ani/tex.png）与原版完全一致，解析器无需特殊适配。只需解析器写好后，替换 MOD 的资产目录即可。

---

## 五、推荐实施路径

### Phase 1 — 离线资产转换工具（快速出效果）

```
工具: UKWAssetConverter.exe（独立命令行工具）
输入: UKW PowerUp Kit 原始资产目录
处理:
  1. XOR 解密 .hod/.mpd
  2. 解析 .x → 写入 .dxmesh + 导出材质信息
  3. 解析 .spt(场景) → 生成 scene.json
  4. 拷贝 .dds/.png 到 Content/ 目录
输出: Content/Scenes/City/ 目录，引擎可直接加载
```

### Phase 2 — 运行时渲染验证

```
使用现有渲染管线：
  OpaqueRenderer    → 建筑/瓦片（PBR deferred）
  SkinnedRenderer   → 机体（蒙皮 + 骨骼）
  ShadowRenderer    → 方向光阴影
  SkyRenderer       → 天空盒
  WaterRenderer     → 水面
  SSAO + Lighting   → 延迟光照合成
```

### Phase 3 — 运行时即时解析（长远）

将 .x/.hod/.spt/.mpd 解析器内建到 AssetLoader/EcsSystem 中，省去离线步骤。

---

## 六、已知风险

1. **.x 解析器复杂度**: DirectX XFile v3.03 二进制模板化格式，需处理 9 种 GUID 模板 + 嵌套结构
2. **顶点格式差异**: .x 中蒙皮权重格式为 D3DCOLORtoUBYTE4 编码，需解码为标准 float4+uint4
3. **Shift-JIS 编码**: .spt 为 Shift-JIS，需要编码转换处理
4. **PNG 纹理加载**: AssetLoader 当前仅支持 DDS，需扩展 PNG 支持
