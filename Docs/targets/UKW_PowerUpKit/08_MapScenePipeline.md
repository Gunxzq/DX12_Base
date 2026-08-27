# UKW 地图场景管线：mpd2scene（拆解 + 合成）方案

> 日期: 2026-08-03
> 状态: 📋 规划定案（待用户确认后实施）
> 关联: `05_MPD_Format_Analysis.md`（MPD 权威结构 + 10 图实测）、`06_SubMeshPipeline.md`、`07_EngineAssetPipeline.md`（FBX 唯一来源）、`Docs/architecture/assets/AssetSpecification.md`（原子资产）、`Docs/architecture/rendering/SubMeshMaterialSlots.md`（#22/#23/#24 材质槽）

---

## 一、目标

把 UKW 地图（MPD + 同目录 .x/.dds/Script.spt）转换为引擎可加载的**场景资产**：

- **拆解**：每个唯一 piece 的 .x → 子网格 dxmesh + 合并重复材质 .mat + 纹理 .dds（材质纹理全保留）
- **合成**：按 MPD 解析结果（对象实例矩阵）+ SPT 环境（天空/雾/光/水/BGM/Hit）→ 一步输出 **scene.json**，符合 `Schemas/scene.schema.json` 验证架构

**复用颗粒度 = piece 级**：40 个唯一 piece dxmesh 被 7623 个实例共享（实例化），顶点驻留从 102 万降到 4485（228× 缩减），不做子网格级资产去重（无收益且破坏复用性）。

## 二、管线总览

```
map/City/  ──────────────────────────────────────────────┐
  map.mpd（对象实例：pieceID + 矩阵，明文/XOR 两种）        │
  mapChip03.x / tree.x / ...（40 唯一 piece，多材质）       │
  *.dds（纹理，XOR 0x0B7E7759 加密）                       │
  Script.spt（环境：天空/雾/光/水/BGM/Hit）                │
──────────────────────────────────────────────────────────┘
                              │ AssetTool mpd2scene <map_dir> <out_dir>
                              ▼
┌───────────── 拆解阶段 ─────────────┐   ┌────────── 合成阶段 ──────────┐
│ 每唯一 piece .x ─assimp─→ 子网格    │   │ MPD 7623 实例（矩阵→TRS）     │
│   dxmesh（SubMesh 表保留材质段）    │   │ + SPT 环境注入                │
│   材质段 → 合并重复材质 → .mat      │   │ ─→ scene.json（符合 schema）  │
│   纹理 → XOR 解密 → .dds           │   │                              │
└────────────────────────────────────┘   └──────────────────────────────┘
```

## 三、拆解阶段（x → 纹理/材质/dxmesh）

### 3.1 流程

1. **解析 MPD**（`MPDSceneParser`，权威结构）：唯一 piece 名列表 + 全部对象实例（pieceID + 16 float 列主序矩阵）
2. **每唯一 piece 拆解**（`XFileParser`/assimp）：
   - assimp 按材质自动拆出 `XFileMesh`（子网格），每段含独立 positions/normals/texcoords/indices + `XFileMaterial`（faceColor/power/specular/emissive/textureFilename）
   - 合并为单个 dxmesh：顶点/索引拼接，`DxMeshSubMesh` 表（indexOffset/count/vertexOffset）记录每个材质段的索引范围
3. **材质去重合并**：所有 piece 的所有材质段按内容（faceColor/specular/emissive/power/textureFilename）hash 去重 → 唯一材质表（City 77 材质段 → 去重后更少）
4. **纹理**：textureFilename → 实际 .dds（XOR 0x0B7E7759 解密，`XORCipher`）→ 输出 `Textures/`

### 3.2 输出文件（City 示例）

```
City/
  Meshes/mapChip03.dxmesh      ← 1 piece = 1 dxmesh（含 SubMesh 表）
  Meshes/tree.dxmesh
  ...（40 个唯一 piece）
  Materials/mat_xxx.mat        ← 合并去重后的唯一材质（MaterialDesc JSON）
  Textures/*.dds              ← XOR 解密后的纹理
```

- dxmesh 用 `DxMeshStaticVertex`（44B，position/normal/tangentU/texC）——UKW 地图无骨骼
- 材质映射复用 `XFileMaterial::ToMaterialDesc()`（已实现：baseColor/emissive/roughness←power/metallic←specular/textures.baseColor）
- 合并重复材质 = 材质内容 FNV-1a hash 去重（`MaterialDesc.hash` 字段预留）

### 3.3 不做的事

- ❌ 子网格拆成独立 dxmesh 文件（复用单位是 piece，子网格只作为 dxmesh 内部材质分段）
- ❌ 子网格级跨 piece 资产去重（各 piece 子网格顶点组合不同，无复用收益）
- ❌ 顶点焊接（assimp 焊接破坏材质/纹理边界，且实例化已拿到 228× 缩减）

## 四、合成阶段（scene.json）

### 4.1 结构映射（符合 `Schemas/scene.schema.json`）

| scene.json 节 | 内容 | 来源 |
|:---|:---|:---|
| `version` | 1 | 固定 |
| `baseURL` | "Content/City" | 输出目录 |
| `metadata.name` | 地图名（如 "City"） | MPD 目录名 |
| `sceneEnvironment.ambient.ambientLight` | [r,g,b,a] | SPT `SetLightColor(255,255,255)` |
| `sceneEnvironment.skybox` | 纹理 + 颜色 + 几何 | SPT `LoadSkyXFile(Sky.x,194,214,240)` → 颜色 RGB(194,214,240)/255；几何用程序化 `{"type":"cube"}` 或 `{"type":"sphere"}` |
| `dependencies.meshes` | `{pieceKey: "Meshes/xxx.dxmesh"}` | 40 个唯一 piece |
| `dependencies.textures` | `{texKey: "Textures/xxx.dds"}` | 去重纹理 |
| `materials` | **内联展开** `MaterialDefinition`（shader/params/textures），纹理引用指向 dependencies.textures 的 key | 合并去重后的材质表（**不可用 .mat 文件引用——SceneLoader 材质/纹理均内嵌**） |
| `entities[]` | 7623 个实例实体（transform + mesh + opaque） | MPD WorldGrid 对象 |

### 4.2 实体生成（每 MPD 对象一个实体）

```json
{
  "name": "mapChip03_0001",
  "components": {
    "transform": {
      "position": [px, py, pz],
      "rotation": [qx, qy, qz, qw],
      "scale": [sx, sy, sz]
    },
    "mesh": {
      "geometry": "mapChip03",
      "materials": ["mat_xxx", "mat_yyy", ...],
      "receivesShadow": true
    },
    "opaque": null
  }
}
```

- **矩阵 → TRS**：MPD 16 float 列主序矩阵 → position（m03,m13,m23）+ rotation（四元数，从 3×3 部分提取）+ scale（列长度）
- **坐标系：右手 Y-up → 引擎左手系 Y-up（翻转 Z）**（2026-08-03 用户定案，参考机体管线）：
  - 顶点/法线/切线 Z 取反：`z = -z`（复用 `FbxMeshConverter.cpp` §5 `leftHanded` 实现，与 importrobot 一致）
  - 索引绕序翻转（保持面朝向）：`(i0,i1,i2)` → `(i0,i2,i1)`
  - 实例矩阵平移列同步：`m03/m13/m23` 的 Z 取反，旋转/缩放列同规则（或整体矩阵 Z 列取反）
- **材质槽数组**：`mesh.materials[]` 长度 = dxmesh SubMesh 数，[i] 对应第 i 个子网格（材质槽模式 #22/#23/#24 定案语义），相同材质可重复出现
- **标签组件**：`opaque`（null 表达存在）；透明 piece 用 `transparent`（判定依据：对象脚本 `@AlphaTestFlag` 或子网格材质 alpha < 1）
- **Sky.x（piece 198）/ Hit.x（piece 199）不在此处生成**（MPD 无实例），由 SPT 注入（见 4.3）

### 4.3 SPT 环境注入（转换范围收窄，2026-08-03 用户定案）

**不转换的内容**：Hit 碰撞盒、item 出生点（PopInfo）、天空盒网格、point 光源等一律不转换。

| SPT 指令 | scene.json 落点 |
|:---|:---|
| `LoadSkyXFile(Sky.x,194,214,240)` | `sceneEnvironment.skybox`：**直接利用同目录 Sky.png**（独立 PNG 结果）→ `dependencies.textures`；几何用**程序化** `{"type":"cube"}`（或 sphere）——引擎 skybox 程序化驱动，只需纹理；颜色 RGB(194,214,240)/255 作兜底 |
| `SetLightColor(r,g,b)` | `sceneEnvironment.ambient.ambientLight` |
| `LoadHitXFile(Hit.x)` | ❌ 不转换（战斗边界不做场景实体） |
| `LoadWaterXFile(Sea.x)` | ❌ 暂不转换（水面留待 Water 系统接入） |
| `BgmFileName(0,fuse.ogg)` | ❌ 不转换（音频） |
| `MapSetting/MapSettingEx` | ❌ 不转换（地块属性语义未确认） |

### 4.4 schema 校验

- 输出 scene.json 用 `Schemas/scene.schema.json` 校验（draft-07），确保引擎 `SceneLoader::Parse*` 可加载
- 引擎侧加载链路：`AssetManager::Load` → `SceneLoader::Parse*`（from_json 四端一致性，见 .atomcode.md #23）→ `SceneConstructor::ConstructEntity`（Desc → ECS）
- **定案（2026-08-03 用户确认）**：scene.json 的 `materials` 必须**内联展开** `MaterialDefinition`——**SceneLoader 的材质与纹理均内嵌**（纹理引用指向 `dependencies.textures` 的 key，非 .mat 文件路径），不输出也不引用独立 .mat

## 五、渲染衔接（实例化）

- 40 个 dxmesh（含 77 材质段）为显存驻留资产；7623 实例 = ECS 实体（Transform + Mesh 组件引用同一 dxmesh）
- 渲染：`RenderSlotCache` 按 shaderType 分桶 → Builder 收集实例 → 实例化绘制（InstanceBuffer 存矩阵）或每 piece 分组 draw
- 剔除：CulledSet 八叉树粗筛（网格分块）→ 每帧只 draw 可见格内实例（MPD grid 100×100 是天然分块）
- 顶点驻留 4485（City），显存 ~0.2MB 几何 + ~0.5MB 实例矩阵

## 六、CLI 命令设计（待实施）

```
AssetTool mpd2scene <map_dir> <out_dir>
  map_dir:  含 map.mpd + *.x + *.dds + Script.spt 的目录（如 map/City）
  out_dir:  输出 Content/City/{Meshes,Materials,Textures,Scenes/City.scene.json}
```

实现要点：
- 复用 `MPDSceneParser`（权威结构）+ `XFileParser`（assimp 子网格）+ `XORCipher` + `XFileMaterial::ToMaterialDesc`
- 新增 `MapSceneConverter`（Core 文件）：拆解（子网格合并 + 材质去重）→ 合成（scene.json）
- 材质去重：FNV-1a 内容 hash → `MaterialDesc.hash`；`dxmesh` 写入用现成 `DxMeshWriter`
- 矩阵→TRS：`DirectXMath` 分解（XMMatrixDecompose）

## 七、实施定案（2026-08-03 用户确认，原待确认项全部关闭）

1. **materials 载体** ✅ 定案：scene.json `materials` **内联展开** `MaterialDefinition`——**SceneLoader 材质/纹理均内嵌**，不输出独立 .mat、不引用 .mat 路径（纹理引用指向 `dependencies.textures` 的 key）
2. **Hit 落点** ✅ 定案：**不转换**（Hit 碰撞盒、item 出生点、天空盒网格、point 光源等一律不转换，不做场景实体）
3. **透明 piece** ✅ 定案：`transparent` 标签判定依据 = 对象脚本 `@AlphaTestFlag` 或子网格材质 alpha < 1
4. **Sky 落点** ✅ 定案：**直接利用同目录 Sky.png**（独立 PNG 结果）→ `dependencies.textures`；几何**程序化** `{"type":"cube"}`（或 sphere）——引擎 skybox 程序化驱动，只需纹理；`LoadSkyXFile` 颜色 RGB(194,214,240)/255 作兜底
5. **坐标/旋转** ✅ 定案：**右手 Y-up → 引擎左手系 Y-up（翻转 Z）**——顶点/法线/切线 Z 取反 + 索引绕序翻转 `(i0,i2,i1)`，复用机体管线 `FbxMeshConverter`/`importrobot` 的 `leftHanded` 实现

---

## 八、区块化聚合（2026-08-05 定案，2026-08-10 修订并已实施：区块归空间哈希生成）

> **2026-08-10 修订（已实施）**：区块（空间哈希块）与集群（BlockComponent）职责分离（`CullingBlueprint.md §4.3`）——
> **场景构建器（Phase C）不再生产 BlockComponent**（✅ 已移除，编辑器字段/schema 未准备，集群功能暂缓）；
> 区块由**剔除层空间哈希模块**按 `blockConfig` 自动生成（✅ `SpatialHashGrid::SetBlocks` + Editor OctreeCulling
> 构建时分组），格存成员实体 ID；块展开缓存归剔除层（✅ `RenderSlotCache::m_blockExpanded` 已迁出）。
> 以下 §8.1-8.7 保留 2026-08-05/08-06 历史定案原文，
> 标注 ⚠️ 的条目为已被 2026-08-10 修订取代的表述。
>
> 动机与收益详见 `Docs/bugs/BugFix_Editor_StaticDirtyState_And_OutlinerIssues.md` §八
> 关联：`Engine/ECS/Core/Components/Block.h`（BlockComponent——集群 Phase C，2026-08-10 起不再由场景构建器生产）、
> `Editor/EditorLib/Scene/EditorSceneManager.cpp`（⚠️ Phase C 已移除——区块划分迁入剔除层空间哈希模块）

### 8.1 背景：实体数量级爆炸

- City4 场景 15489 个 ECS 实体（tree 8404 + tree5/tree2 3383 + 广告牌 2798 + 建筑 ~900）——**94% 是树与广告牌**
- 单实例极小（tree = 24 顶点/14 三角形），视锥剔除一个微型网格的开销可能比绘制还大
- **剔除解决不了数量级问题**（即便剔除一半仍有 7000+ 实体遍历/上传）——需要在数据表达层处理

### 8.2 全图分块实测（10 张地图，500 单位）

| 地图 | 对象数 | 非空格 | 唯一 piece | 世界范围 | 500块数 | 1000块数 |
|:--|:--:|:--:|:--:|:--|:--:|:--:|
| City | 7623 | 604 | 40 | 810×750 | 4 | 1 |
| City2 | 7594 | 604 | 40 | 810×750 | 4 | 1 |
| City3 | 3369 | 152 | 33 | 763×763 | 4 | 1 |
| City4 | 15618 | 902 | 28 | 1050×858 | **5** | 2 |
| City5 | 9850 | 601 | 30 | 834×720 | 4 | 1 |
| City_tac | 9191 | 1157 | 29 | 2100×2070 | **36** | 9 |
| In | 896 | 324 | 15 | 510×510 | 4 | 1 |
| In2 | 350 | 53 | 19 | 210×210 | 4 | 1 |
| moon | 291 | 97 | 109 | 270×270 | 4 | 1 |
| skyland | 6552 | 704 | 72 | 750×884 | 4 | 1 |

> 分析脚本：`.atomcode/tmp/mpd_blocks_analyze.js`（权威结构解析 + XOR 解密 + 分块统计）。
> **注意**：矩阵 16 float 必须**循环内推进偏移**（mpd_verify.js 旧写法 off 未推进导致全部误读为单位阵——mpd_final.js 已修正）。

### 8.3 块大小定案：500 单位

- **1000 单位过大**：9/10 地图退化为 1 块（整图一个实体，块级剔除失效）
- **500 单位是甜点**：8/10 地图 = 4 块（2×2），块内 ~1000-4600 实例；City_tac（2100×2070 大图）36 块——唯一"超视距"场景，块级剔除真正生效
- ⚠️ 与现有 `EditorSceneManager.cpp:983` Phase C 的 500 单位区块划分**完全一致**——复用现有实现，零新概念（2026-08-10 修订：区块划分迁入剔除层空间哈希模块，Phase C 不再产出 BlockComponent）

### 8.4 聚合收益

| 地图 | ECS 实体数 | 聚合后块数 | 减少 |
|:--|:--:|:--:|:--:|
| City4（最大） | 15618 | 5 | **-99.97%** |
| City（典型） | 7623 | 4 | -99.95% |
| City_tac（大图） | 9191 | 36 | -99.6% |

- 遍历/get/管理开销降、剔除/渲染以块为单位（对齐"超视距不需要剔除太多内容"）
- 权衡：聚合牺牲剔除精度（块内不可见内容也处理）——PVS/遮挡计算补偿（§八 用户定案）

### 8.5 落地路径（资产侧聚合）

- ⚠️ **MapSceneConverter 按 500 单位输出块实体**（每块持 piece 句柄 + 实例矩阵数组），`.scene` SOA 二进制同步瘦身（15489 条实例记录 → 块内数组）（2026-08-10 修订：区块数据由剔除层空间哈希模块组织，块内实例矩阵仍保留）
- ⚠️ 块实体照常入 Editor 空间哈希：小图块级粗筛≈全可见（无精度损失），City_tac 大图块级剔除生效（2026-08-10 修订：空间哈希按 `blockConfig` 生成区块、格存成员实体）
- 实例矩阵在块实体上保留（实例化渲染仍拿到 228× 显存缩减），不做顶点焊接（破坏材质/纹理边界，§三 定案）

### 8.6 分层剔除衔接（2026-08-06，见 `CullingBlueprint.md` 蓝图）

> 认知澄清：**ECS 实体数减少（15489→5 块），渲染实例数据量不减（仍 15489 个实例矩阵）**——
> 聚合解决 ECS 遍历/管理开销，**精细剔除从 Builder（CPU）下放 GPU**（Compute 逐实例视锥 + 间接绘制），两者正交衔接。

```
L1 CPU 块级粗筛（✅ 已有）：空间哈希 → 块视锥 → cullDistance → CulledSet（≤5 块）
  → L2 GPU 实例级剔除（📋 待建）：可见块实例矩阵上传（静态一次）→ Compute 剔除 → IndirectArgs
     → DrawIndexedInstancedIndirect（树/广告牌 ~11787 实例走此路径）
  → L3 遮挡（🔭 远期）：HZB / 保守化 PVS（只剔小物体，排除地块）
```

- 树本质是"略微旋转的交叉 quad"（24 顶点/14 三角形），公告牌表达成立 → L2 理想输入
- 建筑（bill00~08，~900）半静态：矩阵持久化，损伤 `_d` 切换走渲染项替换，不进 L2
- 动态物（机体/特效 <50）CPU 视锥 + 每帧上传，被静态世界遮挡（L3 远期）
- **集群职责（2026-08-06 定案，`CullingBlueprint.md` §4.2）**：BlockComponent = **纯剔除豁免器**（对抗远近裁剪面，非对抗实体数——实体数已由块聚合解决）。`forceVisible` 只让**块实体进候选集**（粗筛豁免），块内实例的 L2 GPU 视锥剔除**照常**——集群与区块/L2 正交可叠加。适用山/远距建筑群/地形边界（2026-08-10 修订：集群暂缓——场景构建器不再生产 BlockComponent，编辑器字段/schema 未准备）
- 完整数据流/缓冲布局/实施阶段见 `Docs/architecture/rendering/CullingBlueprint.md`

### 8.7 块配置化（2026-08-06 定案 + 阶段 0 落地 ✅）

> 原则：**场景 JSON 可配置块内容；JSON 缺失时按地图范围自动推导**——对齐 UE World Partition 的 cell 尺寸可配置思路（默认 12800/6400，按项目调，不写死）。
> **状态**：阶段 0（四端 + schema）已落地（2026-08-06），代码待人工编译验证；快照见 `Docs/snapshots/GPUDriven_Snapshot_20260806.md`。

#### 8.7.1 scene.json 新增 `blockConfig`（可选节，✅ 已落地）

```json
{
  "blockConfig": {
    "cellSize": 500,          // 块边长（可选；缺失则按地图范围自动推导）
    "blocksPerAxis": 4,       // 每轴目标块数（与 cellSize 二选一，cellSize 优先）
    "minCellSize": 100,       // 推导下限（小图防过度细分）
    "maxCellSize": 1000       // 推导上限（大图防整图一块）
  }
}
```

- 缺失 `blockConfig` → **加载时自动推导**：`cellSize = clamp(mapExtent / blocksPerAxis)`，`blocksPerAxis` 默认 4（对齐 §8.3 实测：500 单位块在 8/10 地图 = 2×2 = 4 块）
- 推导公式/阈值参照 §8.3 实测表：City_tac（2100×2070）→ 36 块，是唯一"超视距"大图，块级剔除真正生效；In（210×210）小图自动回落下限
- **不硬编码**：引擎内部 `SpatialHashGrid::m_cellSize=250`（查询单元）与块大小（数据组织单元）分离，块由场景决定

#### 8.7.2 引擎侧加载链路（四端一致性，规则 #23，✅ 阶段 0 已落地）

| 端 | 改动 | 状态 |
|:--|:--|:--:|
| `SceneDescription.h` | 新增 `BlockConfigDesc`（cellSize/blocksPerAxis/minCellSize/maxCellSize，默认 0 = 未配置）+ `to_json`/`from_json`；`SceneDescription::blockConfig` 可选字段 | ✅ 0a |
| `SceneLoader.cpp` | 新增 `ParseBlockConfig`（`j.contains("blockConfig")` 检查，缺失则全 0 = 推导模式）+ SaveToJSON 输出 | ✅ 0b |
| `SceneConstructor` | 加载时读 blockConfig：已配置直接用；未配置 → 从实体世界范围推导 cellSize（`clamp(mapExtent/blocksPerAxis)`）→ ⚠️ Phase C 消费 `blockCellSize`（复用 `EditorSceneManager.cpp` Phase C 划分逻辑——2026-08-10 修订：区块划分迁入剔除层空间哈希模块，blockConfig 推导保留） | ✅ 0c（待人工编译） |
| `ExportToDescription` | 编辑器保存时写回 `blockConfig`（`SceneSnapshot` 缓存 + 固化推导结果，下次加载零重算） | ✅ 0d |
| `Schemas/scene.schema.json` | `blockConfig` 属性定义（4 字段，JSON 校验通过） | ✅ 0e |

#### 8.7.3 与空间哈希/剔除的衔接

- ⚠️ 块实体入空间哈希 → 块级 CulledSet → 桶存块条目 → 块内实例矩阵作为 GPU 剔除输入（详见 `CullingBlueprint.md` §四）（2026-08-10 修订：空间哈希格存成员实体 → CulledSet 输出成员实体 → 块展开缓存归剔除层，RenderSlotCache 回归纯材质槽）
- `blockConfig.cellSize` 是**数据组织单元**（区块划分，2026-08-10 起由空间哈希模块生成）；空间哈希格子（`SpatialHashGrid::m_cellSize`）是**查询单元**——两者解耦：块跨格子正常（格子级视锥剪枝仍生效），查询单元可保持 250 或随块大小联动

---

## 九、流式加载结论（2026-08-05 定案：类 EXVS 不需要流式）

> 关联：`Docs/architecture/core/AsyncPipelineResponsibilities.md` §流式加载、
> `Docs/architecture/scene/SceneStateMachine.md`（`stream` 语义）、`SceneFileAndLoading.md` §2.6

### 9.1 块体积实测（500 单位聚合后）

| 地图 | 块实例 SOA 二进制 | 块实例 JSON | 全局共享资产 |
|:--|:--:|:--:|:--|
| City4 大块（4611 实例） | **252KB** | 3.3MB | 17 网格 / 19 纹理 |
| City4 中块（3928） | 215KB | 2.8MB | 同上 |
| City 大块（2635） | 144KB | 1.9MB | 29 网格 / 27 纹理 |
| City 小块（560） | 31KB | 0.4MB | 同上 |

### 9.2 判断标准澄清：流式加载看"地图规模"，不是"块体积"

- 08 年原版：256² DDS + 低模 → 全图 ~3MB，一次加载 <100ms——**没有流式需求**
- 未来 PBR：2K 多通道纹理 → 全图可能 100MB+，异步加载 1-3s 可接受——**仍不构成流式理由**
- 流式加载的动机是**地图规模超过一次加载的容忍度**（开放世界/大地图漫游），不是块体积大小

### 9.3 定案：类 EXVS 竞技场**不做流式**

- 类 EXVS = 2v2 竞技场对战，地图固定几个小竞技场，**无跨区域漫游** → 流式加载要解决的问题不存在
- 整图一次加载完全可接受：当前 4 块 ~300KB，PBR 化后 ~100-200MB 异步加载 + 战斗前加载界面，体验无差
- 08 年原版 `LoadMapData` 就是一次加载整图 → 开打，没有流式
- **StreamingLoader（SceneManager.md P3）长期搁置**；docs 的 `stream` 语义 / LoadBatch 编排 / 相机接近触发已预留，将来若做开放世界可复用（资产常驻 + 实例数据流式）

### 9.4 块化的真正价值（与流式无关）

| 用途 | 必要性 | 说明 |
|:--|:--:|:--|
| 实体聚合（15489 → 4~36 块） | ✅ 必要 | ECS 实体数爆炸是当前唯一真实痛点 |
| 剔除单位（City_tac 36 块） | ✅ 必要 | 2100×2070 超视距，块级剔除生效 |
| 流式加载单元 | ❌ 不必要 | 竞技场小图，一次加载即可 |
