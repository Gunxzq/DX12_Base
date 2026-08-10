# UKW 地图场景管线：mpd2scene 实施快照 (2026-08-03)

> 依据：`Docs/targets/UKW_PowerUpKit/08_MapScenePipeline.md`（2026-08-03 定案，实施定案项全部关闭）
> 关联：`05_MPD_Format_Analysis.md`（MPD 权威结构）、`07_EngineAssetPipeline.md`（FBX 唯一来源）、`Schemas/scene.schema.json`（场景 schema）
> 状态：✅ 管线已实现并跑通；✅ 6 个 PNG 纹理转换问题已修复（根因：魔数判断符号扩展 bug，详见第四节）

---

## 一、实施内容（代码已提交工作区，编译通过）

| 文件 | 改动 |
|:-----|:-----|
| `AssetTool/Core/MapSceneConverter.h` | **新增**。`MapSceneOptions`（mapDir/outDir/leftHanded）、`MapSceneResult`（pieceCount/instanceCount/materialCount/textureCount/scenePath/error）、`MapSceneConverter::Convert()` |
| `AssetTool/Core/MapSceneConverter.cpp` | **新增**。mpd2scene 全管线：拆解（piece .x → 子网格 dxmesh + 材质去重 + 纹理解密）+ 合成（MPD 实例 + SPT 环境 → scene.json） |
| `AssetTool/CLI/main.cpp` | 注册 `mpd2scene <map_dir> <out_dir>` 命令（include + CommandMPD2Scene + PrintUsage + main 分发） |
| `CMakeLists.txt` | `ASSET_TOOL_SHARED_SOURCES` 登记 MapSceneConverter.cpp/.h |

## 二、管线设计要点（对齐 08 文档定案）

### 2.1 拆解阶段
- **唯一 piece** = 被 MPD 对象实例引用的 `visualMesh`（City 实测 40 唯一 piece，7623 实例共享）
- 每 piece `.x` 经 `XFileParser`（assimp，按材质自动拆子网格）→ **合并为单 dxmesh**：顶点/索引拼接 + `DxMeshSubMesh` 表（indexOffset/indexCount/vertexOffset），用 `DxMeshWriter` 写静态顶点（44B，无骨骼）
- **左手系**（右手 Y-up → 引擎左手系 Y-up，复用 FbxMeshConverter §5）：顶点/法线/切线 Z 取反 + 索引绕序翻转 `(i0,i2,i1)`
- **材质去重**：子网格材质经 `XFileMaterial::ToMaterialDesc()` → 内容 FNV-1a hash 去重 → `mat_<hash12>`，**内联展开**于 scene.json（定案：不输出独立 .mat）
- **纹理**：XOR 解密（`DecryptOrCopyDDS` 处理 .dds；新增 `ConvertImageToDDS` 处理可能加密的 .png/.bmp）→ `Textures/`，纹理引用指向 `dependencies.textures` 的 key

### 2.2 合成阶段
- scene.json 符合 `Schemas/scene.schema.json`：version=1 / baseURL="Content/City" / metadata.name / sceneEnvironment（ambient + skybox）/ dependencies（meshes/textures）/ materials（内联）/ entities[]
- 每 MPD 对象 → 实体：`transform`（矩阵→TRS，`XMMatrixDecompose`，MPD 列主序 16 float 逐位复制 = M^T）+ `mesh`（geometry + materials[] 材质槽数组，长度 = SubMesh 数）+ `opaque`/`transparent` 标签
- **透明判定**：对象脚本含 `@AlphaTestFlag` 或子网格材质 alpha < 1
- **SPT 环境注入**：`SetLightColor` → ambientLight；`LoadSkyXFile` → skybox（Sky.png 纹理 + 程序化 `{"type":"cube"}` 几何 + LoadSkyXFile 颜色 RGB/255 兜底）
- **不转换**（定案）：Hit 碰撞盒 / item 出生点 / 天空盒网格 / point 光源 / 水 / BGM / MapSetting；Sky.x（piece 198）/ Hit.x（piece 199）不作为 piece 实体

## 三、验证结果（City 实测，2026-08-03）

```
AssetTool mpd2scene "D:/APP/Ultimate Knight WindomXP PowerUp Kit/map/City" build/CityTest
[mpd2scene] pieces: 40, instances: 7623, materials: 37, textures: 27
[mpd2scene] scene: build/CityTest\Scenes\City.scene.json
```

- ✅ **40 唯一 piece dxmesh 全量生成**（Meshes/*.dxmesh，40 个）
- ✅ **7623 实例实体**（与 08 文档预期一致）
- ✅ **37 去重材质**（内联展开）
- ✅ **27 纹理**输出（Textures/，含 Sky.png；26 个被材质槽引用 + Sky 由 skybox 引用，无悬空引用）
- ✅ **schema 校验通过**（node 逐项核对：version/baseURL/dependencies/materials/entities 全部合法，7623 实体 transform/mesh/材质槽/标签均无悬空引用）

### 关键过程问题与解决（记录备查）

1. **7 个 piece .x 解析失败**（bill01/mapChip03/mapKABE 等，assimp "Unexpected end of file"）→ 根因：源 .x 文件为 **XOR 加密**（头部非 "xof "）。**解密是使用者的任务**——用户手动解密后 40 个 piece 全部解析成功（2026-08-03 会话定案）
2. **MPD 目录名含空格**：真实目录为 `Ultimate Knight WindomXP PowerUp Kit`（PowerUp 连写），MSYS/Windows 参数传递易混淆，调用时需注意
3. **矩阵→TRS 转置方向**：MPD 16 float 列主序（列向量约定 world=M·v，平移列 m03/m13/m23）→ `XMFLOAT4X4` 行主序（行向量约定）**逐位 memcpy 即等价转置**，`XMMatrixDecompose` 提取的平移即 m[12]/m[13]/m[14]（修正了最初写反的 `c*4+r` 转置）

## 四、✅ 遗留问题已解决：6 个 PNG 纹理转换失败（2026-08-03 同日闭环）

mpd2scene 内 `processTexture` 曾对 6 个 PNG 转换失败（WARN "Image is neither plain nor decryptable (bad magic)"），但**单独运行 `png2dds` 对同一文件全部成功**：

```
build03.png / build085_2.png / build04.png / biru_001.png / build06.png / metal00.png
```

### 根因（已修复）
- `MapSceneConverter.cpp` `IsPlainImageMagic` 中 `magic[0] == '\x89'`：`'\x89'` 是 char 字面量，MSVC 下 char 默认有符号（值 -119），与 `uint8_t` 提升后的 +137 恒不相等 → 明文 PNG 魔数检查**永远失败** → 误入 XOR 解密分支 → 报 bad magic
- `png2dds` 直接调 `ConvertPNGToDDS`（stbi 加载，不经过魔数检查），故单独运行全部成功，形成对比
- **修复**：`'\x89'` → `0x89`（整数字面量，无符号比较正确），单行改动；2026-08-03 人工编译验证

### 验证结果（修复后重跑）
```
[mpd2scene] pieces: 40, instances: 7623, materials: 37, textures: 27
```
- 6 个 PNG WARN 全部消失；`textures: 21 → 27`；`materials: 33 → 37`（去重 hash 含 texKey，修复前失败纹理 key 为空串导致材质错误合并）
- scene.json 完整性抽查（node 校验）：27 个纹理无悬空引用，26 个被材质槽引用 + Sky 由 skybox 引用，7623 实体材质槽全部指向已定义材质

### 排除的怀疑点（记录备查）
- 路径解析 / `fs::path::string()` 编码差异：与失败无关
- `texKeyCache` 首次失败缓存空串：仅放大症状，非根因（修复后全部成功不再触发）
- stbi_load 失败 / dst 写入问题：均非根因

## 五、备注

- `mpd2scene` 与旧 `map` 命令（BuildMapScene，MPD 已弃用的采样方案）并存；08 文档定案后，新地图应使用 `mpd2scene` 权威管线
- 源 .x/.dds/.png 解密由使用者负责（本次会话定案），工具侧 `XFileParser`/`DecryptOrCopyDDS`/`ConvertImageToDDS` 保留自动解密兜底
- 引擎侧加载链路：`AssetManager::Load` → `SceneLoader::Parse*` → `SceneConstructor::ConstructEntity`，scene.json 需保持四端一致性（见 .atomcode.md #23）
