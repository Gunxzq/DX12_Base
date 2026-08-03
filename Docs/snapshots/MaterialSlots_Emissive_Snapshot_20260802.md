# 材质槽 + Emissive 通道推进快照 (2026-08-02)

> 材质槽（SubMesh 级材质分配）链路打通 + G-buffer Emissive 通道落地 + 顶点布局统一
> 关联：`Docs/architecture/rendering/SubMeshMaterialSlots.md`（设计定案）、`Docs/architecture/rendering/RendererDataDriven.md`（§2.6a 顶点布局前置约束）
> 待办：#22/#23/#24

---

## 一、材质槽链路打通（#22/#24 完成）

### 1.1 数据形态确认

KD-03 为验证载体：`kd-03.dxmesh` 头文件 `subMeshCount=8` ↔ 场景 JSON `mesh.materials[]` 8 个 key ↔ `Materials/kd-03_000..007.mat` 8 个材质，一一对应。**槽位数由 .dxmesh 的 SubMesh 表决定，材质对应由 materials[] 数组承载。**

### 1.2 已落地改动

| 项 | 内容 |
|:---|:-----|
| 场景样本 | `Content/Scenes/async_test.scene.json` 嵌入 `KD03_Slots` 实体（8 槽 + 真实材质参数） |
| schema | `Schemas/scene.schema.json` MeshComponent 支持 `materials[]`（`material` 向后兼容，`required` 降为 `["geometry"]`） |
| 四端 | `MeshDesc.materials[]` / `ParseMesh` / `SceneConstructor` / `ExportToDescription`（此前已数组化） |
| Builder | `OpaqueRenderItemBuilder` SubMesh 展开（`GetSubMeshInfo` + 按槽取材质 + fallback `[0]`） |
| **关键修复** | `Editor.cpp` 创建 `OpaqueRenderItemBuilder` 后补 `SetGeometryManager(m_context->GeometryResourceManager)`——此前 `m_geometryManager` 恒 nullptr，`GetSubMeshInfo` 永远返回空，KD-03 整网格被当 1 个渲染项（`queueItems=3` 而非 10） |
| 诊断日志 | 4 处 `[Diag]`：材质注册 / 槽位解析 / SubMesh 绑定（Builder）/ matSRV 绑定（渲染） |

### 1.3 运行验证

日志确认：材质注册 11 个全成功、`KD03_Slots` 8 槽全有效（`gpuIdx` 与 handleIndex 一致）、`matSRV.ptr` 非 0、`queueItems=10`（Ground 1 + TestCube 1 + KD03 8 子网格）。

---

## 二、索引双重偏移问题与方案 A 定案（2026-08-02）

### 2.1 症状

KD-03 渲染后 **5 个子网格位置丢失**（sub[3..7] 越界：`startVertex + 绝对索引 > 3045`）。

### 2.2 根因

AssetTool（RobotMerger/FbxMeshConverter）拼接时已将各部件索引 `+ vOffset` **绝对化**，同时 SubMesh 表记录 `vertexOffset = vOffset`。引擎端把 `startVertex` 作为 `DrawIndexedInstanced` 的 **BaseVertexLocation** 传入 → **二次偏移**（顶点索引 = 绝对索引 + startVertex → 越界/错位）。

### 2.3 定案（对齐大型引擎）

- **索引恒为绝对索引**（指向全局顶点数组），`subMesh[i]` 只是 IndexBuffer 的一段连续区间
- **BaseVertexLocation 恒为 0**，`startVertex` 仅作记录不参与绘制
- **网格间不打包**：每个 `.dxmesh` 资产独立 VertexBuffer/IndexBuffer，子网格共享顶点缓冲
- 详见 `SubMeshMaterialSlots.md` §2.3

### 2.4 落地

| 位置 | 改动 |
|:-----|:-----|
| `OpaqueRenderItemBuilder.cpp` | SubMesh 展开 + 旧路径 `startVertex` 传 0 |
| `OpaqueRenderer.cpp` / `ShadowRenderer.cpp` / `SkinnedRenderer.cpp` | `DrawIndexedInstanced` BaseVertexLocation 恒传 0（`(void)startVertex`） |

---

## 三、顶点布局统一（跨渲染器前置约束）

### 3.1 问题

`DxMeshSkinnedVertex` 头部为 `position/tangentU/normal/texC`（M3dVertex 旧布局），与 `DxMeshStaticVertex` 的 `position/normal/tangentU/texC` **顺序互换**。蒙皮网格走 Opaque 渲染器时 `NORMAL@12` 读到硬编码 `tangentU=(1,0,0)` → 法线全同、光照错误、颜色随视角变化甚至全黑（RenderDoc 实测）。

### 3.2 定案

头部（position/normal/tangentU/texC，44B）静态/蒙皮**完全一致**，蒙皮仅尾部追加 `boneWeights@44 / boneIndices@60(R8G8B8A8_UINT)`。对齐 Unity 统一 `VertexAttribute` / UE `FVertexFactory` 头部一致。详见 `SubMeshMaterialSlots.md` §2.3a、`RendererDataDriven.md` §2.6a。

### 3.3 落地

| 位置 | 改动 |
|:-----|:-----|
| `DxMeshFormat.h` | `DxMeshSkinnedVertex` 字段顺序 → position/normal/tangentU/texC/boneWeights/boneIndices |
| `SkinnedRenderer.cpp` | input layout `NORMAL@12 / TANGENT@24`（头部对齐 Opaque） |
| AssetTool | 结构体赋值按成员名自动跟随，无需改赋值语句 |
| 资产重导 | **KD-03.dxmesh 必须重跑 `fbxs2dxmesh`**（二进制写死字段顺序）；静态网格 44B 不变无需重导 |

### 3.4 已重导

`AssetTool.exe fbxs2dxmesh C:/Users/32199/Desktop/kd-03.fbx build/out_kd03` → `Content/Models/KD-03/kd-03.dxmesh`。验证：`normal@12` 为真实法线（非 (1,0,0)），8 子网格 / 3045 顶点 / 15 骨骼 / 8 材质。

---

## 四、Emissive 通道（G-buffer 4→5 RT，方案 A）

### 4.1 动机

UKW Basic.fx/Reflection.fx 用环境贴图反射解决帽檐下发光——延迟管线的标准做法是 **emissive 通道**：自发光直接输出，不依赖光源角度、不依赖环境贴图。参考 `D:/APP/Ultimate Knight WindomXP PowerUp Kit/data/Reflection.fx`（环境 cubemap 反射是前向管线时代的 hack）。

### 4.2 落地

| 位置 | 改动 |
|:-----|:-----|
| `WindowFrameResources.h/.cpp` | G-buffer 4→5（新增 `GBufferEmissive`，`R11G11B10_FLOAT` HDR），`GetGBufferCount()=5` |
| `OpaqueRenderer.cpp` / `SkinnedRenderer.cpp` | PSO `NumRenderTargets=5`，`RTVFormats[4]=R11G11B10_FLOAT` |
| `color.hlsl` / `skinned.hlsl` | GBuffer 输出 `SV_Target4 = matData.Emissive` |
| `lighting.hlsl` | `gEmissiveRT(t24)` 采样，`return ambient + direct + reflection + emissive` |
| `LightingRenderer.cpp/h` | 根签名 slot 6=t24 Emissive（后续槽位顺延），`BeginFrame` 加 `emissiveSrv` |
| `Editor.cpp` | `rtvs[5]`、`OMSetRenderTargets(5)`、clear ×5、屏障 ×5（对称，规则 #10）、传 `GetGBufferSRV(4)` |
| `EditorViewport.h/.cpp` | `m_gbufferRtvHandles[5]`、`SyncGbufferRtvHandles` 循环 `i<5` |

### 4.3 踩坑与修复

| 症状 | 根因 | 修复 |
|:-----|:-----|:-----|
| 崩溃 `Close()` @Editor.cpp:1118 | **对称屏障破坏**：Opaque 入口 `barrierRT` 含 5 个 RT，出口 `barrierBack` 只回退 4 个 → 第 5 个停在 RENDER_TARGET，下帧状态校验失败 | 出口补 `barrierBack(GetGBufferResource(4))` |
| 崩溃 `ResourceBarrier(NULL)` | `WindowFrameResources` getter 越界检查仍 `i>=4`，`GetGBufferResource(4)` 返回 nullptr | getter 边界 4→5（3 处） |
| 编译错误 C2511 | `LightingRenderer.h` 声明加 `emissiveSrv`，`.cpp` 定义未同步 | 定义补参数 |

### 4.4 已知问题（记录）

`WindowFrameResources` getter 越界检查硬编码数量（`i>=4`→`i>=5`），与 `m_gbuffer[5]` 数组、`GetGBufferCount()` 三处魔法数字重复。后续应改为基于 `GetGBufferCount()` / `std::size` 单一事实来源。详见 `SubMeshMaterialSlots.md` §5.3。

---

## 五、环境光与环境反射

### 5.1 环境光调整

`async_test.scene.json`：`ambientLight (0.6,0.6,0.8,1.0) → (0.7,0.7,0.7,2.0)`（等效 `rgb×w=(1.4,1.4,1.4)`）。理由：原 B 通道偏高加重偏蓝；深灰材质背光面 `0.6×0.09≈0.05` 不可见。公式 `ambient = gAmbientLight.rgb × w × albedo × ao`。

### 5.2 反射与 SSAO 现状

- **SSAO 保持关闭**（`Editor.cpp:1088` `ssaoSrv` 传空）——开了只会更暗淡
- **环境反射未全局启用**：`ComputeEnvironmentReflectionDeferred` 依赖 probeIndex（无探针 → 0），符合"不是所有实体都需要反射环境贴图"

---

## 六、Emissive 数据链路修复（FBX + X 路径）

### 6.1 问题

KD-03 眼部材质（.x 原始 Emissive = (241,255,115) ≈ (0.945,1.0,0.451) 黄绿）在管线中丢失：

1. `RobotMergerUtil.cpp` `CreatePartMaterial` 把 emissive **合入 diffuse**（兼容 hack：assimp FBX 导出器写 "Emissive"/Vector3D，Blender 5.2 只认 "EmissiveColor"/Color）→ Blender 里眼部成了普通黄绿材质（emissive=0）
2. `ExtractMaterial`（fbxs2dxmesh）**从未读 emissive** → .mat 无 emissive 字段
3. `.mat` 序列化手写字段（FBX 路径 + importrobot 路径）均漏 emissive

### 6.2 已修复

| 位置 | 改动 |
|:-----|:-----|
| `FbxMeshConverter.cpp` `ExtractMaterial` | 补 emissive **颜色 + 强度**两步（`AI_MATKEY_COLOR_EMISSIVE` + `AI_MATKEY_EMISSIVE_INTENSITY`，社区标准做法） |
| `FbxMeshConverter.cpp` .mat 序列化 | 补 `params.emissive` |
| `XFileParser.cpp` `ToMaterialDesc` | 补 `params.emissive`（emissiveColor → MaterialDesc） |
| `RobotMerger.cpp` .mat 输出 | 补 `params.emissive` |
| `async_test.scene.json` | `mat_kd03_3`（眼部）`emissive = [0.945, 1.0, 0.451, 1.0]` |

### 6.3 验证状态

- `fbxs2dxmesh` 重跑后新 .mat 已有 emissive 字段，但值为 0——**FBX 源头数据确实无 EmissiveColor**（Blender 导出时 emissive 已丢，因合入 hack）
- 引擎侧验证需用 JSON 中眼部 emissive 值（已设）观察恒定发光
- **X→FBX 桥（ani2anim）已修复**（2026-08-02）：assimp 6.0.4 FBX 导出器 modern 段不写 `EmissiveColor`（只写 legacy `"Emissive"/Vector3D`，Blender 5.2 不认），已通过 vcpkg overlay 补丁修复（`vcpkg-overlays/assimp/fbx_emissive_export.patch`，`assimp@6.0.4#2`）。验证：`KD03_anim.fbx` 中 `EmissiveColor`/`EmissiveFactor` 各 9 次（8 材质实例 + 1 模板），修复前仅 1 次模板默认值。详见 `Docs/bugs/BugFix_FBXExporter_EmissiveColor.md`

---

## 七、当前状态与下一步

### ✅ 已打通
- 材质槽链路（JSON → Desc → ECS materialSlots → Builder SubMesh 展开 → 渲染）
- 顶点布局统一（头部 44B 静态/蒙皮一致 + 蒙皮尾部骨骼字段）
- Emissive 通道（G-buffer 5 RT + 光照合成加项）
- Emissive 数据提取（FBX 颜色+强度两步 + X 路径序列化）
- **X→FBX 桥发光保留**（assimp 导出器 EmissiveColor 补丁，`assimp@6.0.4#2`，见 §6.3）

### 📋 待做（未排期）
- **MeshEditor 属性卡**（#23）：材质槽列表编辑 + 双写 `m_entityDescs` 缓存（用户统一审核后推进）
- **蒙皮渲染路径 SubMesh 展开**（Step 4）：`SkinnedRenderItemBuilder` 当前只消费 `materialSlots[0]`；KD-03 走 Opaque 路径观察是临时手段（前置 #62 发光链路已通）
- **Blender 眼部 Emission 修正**（P3 降级）：X→FBX 桥已能写 EmissiveColor 后，仅旧 FBX（合入 hack 时代导出）需要此修正；新管线从 .x 重建 FBX 不再需要
- **WindowFrameResources getter 边界去魔法数字**（`SubMeshMaterialSlots.md` §5.3）
- **TerrainRenderer GBuffer PSO 同步**：当前 4 RT，接入 EditorViewport G-buffer 时需加第 5 通道
- **反射探针接入**：环境反射按需启用（非全局 IBL）

## 八、关键文件

```
Docs/architecture/rendering/SubMeshMaterialSlots.md   ← §2.3a 顶点布局统一、§2.3 绝对索引语义、§5.3 已知问题
Docs/architecture/rendering/RendererDataDriven.md     ← §2.6a 顶点布局前置约束
Content/Scenes/async_test.scene.json        ← KD-03 观察样本（8 槽真实材质 + 眼部 emissive + 环境光）
Engine/Asset/Definitions/Mesh/DxMeshFormat.h ← SkinnedVertex 统一布局
AssetTool/Core/FbxMeshConverter.cpp         ← ExtractMaterial emissive 两步提取 + .mat 序列化
AssetTool/Core/XFileParser.cpp              ← ToMaterialDesc emissive
AssetTool/Core/RobotMerger.cpp              ← .mat 输出 emissive
Shaders/lighting.hlsl                       ← gEmissiveRT + emissive 加项
```
