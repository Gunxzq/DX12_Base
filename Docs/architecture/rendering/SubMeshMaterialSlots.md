# SubMesh 材质槽与材质驱动渲染架构

> 日期：2026-07-29（更新：2026-07-29，最终设计定案）
> 状态：📋 设计定案
> 关联：`Docs/todos/remaining_issues.md` #22/#23/#24

---

## 1. 动机

### 1.1 当前限制

`MeshComponent` 目前只持有一个 `MaterialHandle`，无法表达**同一网格的不同 SubMesh 使用不同材质**的需求。这在以下场景中受限：

- **NPR + PBR 混合角色**：身体用 NPR 卡通材质，衣物用 PBR 材质
- **多材质组合物体**：金属框架+玻璃面、带贴花的物体等
- **SubMesh 级材质替换**：运行时只替换某个部分的材质

### 1.2 参考实现

| 引擎 | 机制 |
|:-----|:------|
| **Unity** | `SkinnedMeshRenderer.materials[]`，与 `sharedMesh.subMeshCount` 一一对应 |
| **Unreal** | `FSkeletalMeshRenderData::RenderSections[]`，材质槽独立 |
| **Cocos** | `ModelComponent.materials[]`，数组长度与 SubMesh 数一致 |

本方案选择 **Cocos/Unity 模式**：按 SubMesh 索引为主列，相同材质可重复出现于多个槽位。

---

## 2. 最终架构

### 2.1 MeshComponent 材质槽数组化

```cpp
struct MeshComponent {
    Resource::LODMeshHandle lodMeshHandle;
    std::vector<Resource::MaterialHandle> materialSlots;  // [subMeshIndex] = MaterialHandle
    bool receivesShadow = true;
    Math::BoundingVolumeVariant localBounds;
    bool IsValid() const { return lodMeshHandle.IsValid(); }
};
```

**关键设计决策**：

- **索引 = SubMesh 索引**，`materialSlots[i]` 对应网格的第 i 个子网格。子网格之间共用材质时，对应槽位存放相同的 MaterialHandle（冗余）。
- **`std::vector<MaterialHandle>` 直接内嵌在 ECS 组件中**，而非通过 MaterialManager 间接引用。原因：
  - 属性卡可以直接遍历 `materialSlots` 并调用 `matMgr->GetMaterial(handle)` 展开显示（一步展开）
  - 替换材料时直接写回 `materialSlots[i]`，不需要经过管理器中间层
  - 序列化走 `m_entityDescs` 缓存的 string key，不依赖 handle 值（见 §3.2）
- **Retain/Release 同单 handle 模式**：拷贝 MeshComponent 时对每个 handle 调 Retain，析构时调 Release。当前单 handle 已有此机制，扩到数组无质变。
- **冗余接受**：JSON 与运行时数组长度 = `subMeshCount`，相同材质可重复。这是用户直觉——"调整第 3 个子网格的材质"直接对应 `materials[2]` 槽位。

**统一语义（2026-08-02 定案）**：
- **所有网格按子网格处理**：`materials[]` 长度 = SubMesh 数；**无子网格表的网格视为 1 个子网格**（整个索引区间），由加载器兜底补 1 条 `SubMeshInfo{0, indexCount, 0}`（`DxMeshLoader` / `MeshLoadTask` / `GeometryResourceManager::RegisterGeometryVariant` 三处兜底），消费方不再判空。
- **材质槽固化为数组**：场景 JSON / ECS / Builder 一律 `materials[]`，**已移除单材质 `material` 兼容分支**（2026-08-02：`SceneDescription::from_json` / `SceneLoader::ParseMesh` / schema / AssetTool 生成处）。
- 旧 ECS `materialHandle` → 新建 MeshComponent 时 `materialSlots = { materialHandle }`

### 2.2 场景 JSON 格式

```json
{
    "name": "Character",
    "components": {
        "mesh": {
            "geometry": "character_mesh",
            "materials": [
                "mat_body_npr",
                "mat_body_npr",
                "mat_hair_pbr",
                "mat_eyes_pbr",
                "mat_eyes_pbr"
            ]
        }
    }
}
```

**`materials[]` 数组长度 = 该网格的 SubMesh 数量**（加载时根据 `.dxmesh` 的 SubMesh 表验证）。相同材质重复出现是预期行为。

### 2.3 SubMeshInfo：网格文件与运行时的信息

`.dxmesh` 文件在 Step 0 扩展后，新增 SubMesh 表。加载到 `TriangleMesh` 后缓存为：

```cpp
// Mesh 的 SubMesh 信息（TriangleMesh 缓存，GeometryResourceManager 可查询）
struct SubMeshInfo {
    uint32_t startIndex;   // 索引区间起点（相对 IndexBuffer 起始，按索引数计）
    uint32_t indexCount;   // 本 SubMesh 索引数
    int32_t startVertex;   // 仅记录语义：本 SubMesh 顶点段起点（AssetTool 拼接时的 vOffset）
};
```

**格式语义约定（重要，与大型引擎对齐）**：

- **索引恒为绝对索引**：`.dxmesh` 的 IndexBuffer 是**整个网格**的全局索引数组，`subMesh[i]` 只是其中一段连续区间 `[startIndex, startIndex+indexCount)`。AssetTool（`RobotMerger` / `FbxMeshConverter`）在拼接时已将各部件索引统一 `+ vOffset` 绝对化（`allIndices.push_back(ms.indices[i] + vOffset)`）。
- **`BaseVertexLocation` 恒为 0**：因为索引已指向全局顶点数组，绘制时**不能再**将 `startVertex` 作为 `BaseVertexLocation` 传入，否则造成**双重偏移**（顶点索引 = 绝对索引 + startVertex → 越界/错位）。`DrawIndexedInstanced` 的 BaseVertexLocation 参数必须传 0。
- **`startVertex` 仅作记录，不参与绘制**：它表示 AssetTool 拼接时该部件顶点的段起点（用于工具链核对/未来按段查询），运行时渲染不消费它。**禁止**把它当 BaseVertexLocation 使用。
- **网格间不打包**：引擎假定每个 `.dxmesh` 资产独立拥有自己的 VertexBuffer/IndexBuffer，**不会**把多个无关网格拼进同一个静态缓冲统一上传渲染。子网格共享所在网格的顶点缓冲，仅通过索引区间区分。

```cpp
struct TriangleMesh {
    // ... 现有字段 ...
    uint32_t flags;                           // 含 DxMeshFlag_Skinned
    std::vector<SubMeshInfo> subMeshes;       // ← 新增
    
    bool IsSkinned() const { return flags & DxMeshFlag_Skinned; }
    uint32_t GetSubMeshCount() const { return static_cast<uint32_t>(subMeshes.size()); }
};

// GeometryResourceManager 新增接口：
const std::vector<SubMeshInfo> *GetSubMeshInfo(GeometryHandle handle) const;
```

### 2.3a 顶点布局统一（跨渲染器前置约束，2026-08-02 定案）

**现状问题**：两个顶点结构头部顺序不一致，是早期 M3dVertex 旧布局（tangent 在前）与静态布局（normal 在前）并存的历史债：

```
DxMeshStaticVertex (44B):  position@0  normal@12  tangentU@24  texC@36   ← 头部正常
DxMeshSkinnedVertex (64B): position@0  tangentU@12 normal@24   texC@36   ← tangent/normal 互换!
```

后果：蒙皮网格若走静态 Opaque 渲染器（按静态布局读），`NORMAL@12` 读到的是蒙皮顶点的 `tangentU`（AssetTool 硬编码为常量 `(1,0,0)`）→ **法线全同、光照错误、颜色随视角变化甚至全黑**。RenderDoc 实测确认（法线数据在视野下全部相同）。

**定案：头部字段均一，尾部差异化**（对齐大型引擎——Unity 统一 `VertexAttribute`，UE `FVertexFactory` 头部一致）：

```
头部（静态/蒙皮共用，44B，与 DxMeshStaticVertex 顺序一致）：
  position@0 | normal@12 | tangentU@24 | texC@36
尾部（差异化追加）：
  静态：无（44B 不变）
  蒙皮：boneWeights@44(16B) | boneIndices@60(4B)  → 64B
```

**改动点**：
- `DxMeshFormat.h` `DxMeshSkinnedVertex` 字段顺序 → position/normal/tangentU/texC/boneWeights/boneIndices（与 M3dVertex 解耦）
- `SkinnedRenderer` input layout 同步改 `NORMAL@12 / TANGENT@24`（与 Opaque 头部一致），`BLENDWEIGHTS@44 / BLENDINDICES@60(R8G8B8A8_UINT)` 尾部不变
- AssetTool 结构体字段顺序变化自动跟随（赋值按成员名），无需改赋值语句

**收益**：
- Opaque/Skinned 渲染器头部完全一致 → 即使走错路径，前 44 字节也能正确读出，不会出现"法线读到常量"的灾难性错位
- 数据驱动 PSO 的几何条件（顶点输入布局）判定更简单：头部统一，差异只在尾部是否含骨骼字段
- 换渲染器可兼容头部，为后续渲染器数据驱动（见 `RendererDataDriven.md`）铺路

**资产重导**：`.dxmesh` 二进制格式写死顶点结构体顺序，改布局必须重跑 AssetTool：
- `KD-03.dxmesh`（蒙皮）→ 必须重导（`importrobot` 重新生成）
- cube/cylinder/ground/torus（静态）→ 不需要（布局不变，仍 44B 头部）

### 2.4 SubMesh 展开阶段

```
ECS Registry
  └─ MeshComponent.materialSlots[] (实体级，每实体一份，索引=submesh)
       │
       ▼
〔SubMesh 展开阶段〕 ← PreRender 中的独立阶段（或 Builder 内部）
  │  从 GeometryResourceManager 读取 mesh 的 SubMeshInfo[]
  │  按索引配对: materialSlots[i] ↔ subMeshes[i]
  │  输出 SubMeshItem[]
  │
  ▼
SubMeshItem[]（可缓存，mesh+slot 不变时不重算）
  ├─ Item[0]: Entity_A, slot=0, submesh=0, mat=body_npr,  startIndex=0,   indexCount=36
  ├─ Item[1]: Entity_A, slot=1, submesh=1, mat=body_npr,  startIndex=36,  indexCount=24
  ├─ Item[2]: Entity_A, slot=2, submesh=2, mat=hair_pbr,  startIndex=60,  indexCount=18
  ├─ Item[3]: Entity_A, slot=3, submesh=3, mat=eyes_pbr,  startIndex=78,  indexCount=12
  ├─ Item[4]: Entity_A, slot=4, submesh=4, mat=eyes_pbr,  startIndex=90,  indexCount=12
  ├─ Item[5]: Entity_B, slot=0, submesh=0, mat=ground_pbr,startIndex=0,   indexCount=18
  └─ ...
```

**容错规则**：如果 `materialSlots.size() < subMeshes.size()`（如旧场景只有一个材质），缺失的槽位使用 `materialSlots[0]` 填充。如果 `materialSlots` 为空，使用无效 handle（Builder 自行跳过）。

### 2.5 Builder 层展开（替代独立阶段）

OpaqueRenderItemBuilder 内联 SubMesh 展开，不引入 PreRender 独立阶段：

```
BuildTyped 当前:
  for 每个实体:
    geoHandle = PickLOD(meshComp.lodMeshHandle)
    materialIdx = matMgr->GetGPUIndex(meshComp.materialHandle)
    BatchKey = {geoHandle, materialIdx, meshComp.startIndex, ...}
    → 1 RenderItem

BuildTyped 改为:
  for 每个实体:
    geoHandle = PickLOD(meshComp.lodMeshHandle)
    subMeshes = geoMgr->GetSubMeshInfo(geoHandle)
    // 统一语义：无子网格 = 1 个子网格（加载器已兜底），subMeshes 恒非空
    for i = 0 .. subMeshes.size():
      matHandle = (i < materialSlots.size() && materialSlots[i].IsValid())
                  ? materialSlots[i] : materialSlots[0] (fallback)
      materialIdx = matMgr->GetGPUIndex(matHandle)
      // startVertex 恒传 0：索引已绝对化，BaseVertexLocation 必须为 0（见 §2.3）
      BatchKey{geoHandle, materialIdx, subMeshes[i].startIndex, 0, subMeshes[i].indexCount}
      → 1 RenderItem (per SubMesh)
```

**合批规则不变**：同 `{geoHandle + materialIdx + startIndex/indexCount}` 合并为一次 Draw Call。相同材质的不同 SubMesh（如 body_npr 在 submesh 0 和 1 出现两次）由于 `startIndex` 不同不会错误合并，但同材质 + 同偏移的跨实体 SubMesh 可合批。

### 2.6 渲染器层（BaseVertexLocation 恒为 0）

所有渲染器（OpaqueRenderer/ShadowRenderer/SkinnedRenderer）通过 `item.startIndex` / `item.indexCount` 绘制。**`startVertex` 不再作为 BaseVertexLocation 传入**——索引已绝对化，`DrawIndexedInstanced` 的 BaseVertexLocation 参数恒传 0（见 §2.3 语义约定）：

```
OpaqueRenderer::Draw:
  mesh = GetGeometry<TriangleMesh>(item.geometryHandle)
  SetVB(mesh->vertexBufferHandle)
  SetIB(mesh->indexBufferHandle)
  DrawIndexed(item.indexCount, item.startIndex, item.startVertex)
```

---

## 3. Handle 系统约束与属性卡展开

### 3.1 三系统的句柄性质

| 系统 | 句柄类型 | 管理器 | 属性卡编辑方式 |
|:-----|:---------|:-------|:--------------|
| 材质 | `MaterialHandle` (32bit) | `MaterialManager` | handle → `GetMaterial()` → 显示名称 → 用户替换 → 回写 handle |
| 几何体 | `GeometryHandle` / `LODMeshHandle` | `GeometryResourceManager` | 只读显示，由 AssetBrowser 拖拽替换 |
| 纹理 | `TextureHandle` | `TextureManager` | 只读显示，由 AssetBrowser 拖拽替换 |

句柄是运行时内部的索引+世代号，**不能直接映射为 UI 可编辑的值**。属性卡编辑材质槽的流程称为**一步展开**：

```
MeshComponent.materialSlots[i] (MaterialHandle)
  → MaterialManager::GetMaterial(handle) → MaterialData
  → 显示 MaterialData.name / MaterialData.shaderType
  → 用户从 AssetBrowser 拖入新材质
  → 新材质的 MaterialHandle 写回 materialSlots[i]
  → 同步更新 m_entityDescs 缓存中的 string key（供 ExportToDescription 使用）
```

### 3.2 序列化：string key ↔ handle 的隔离

```
场景 JSON (string key)          编辑器缓存 (string key)         运行时 (handle)
"materials": ["skin","hair"]    MeshDesc.materials              materialSlots[0..N]
       ↓ load                           ↓ SceneConstructor.resolve
  m_entityDescs[entity].mesh      SceneConstructor::ConstructEntity
  保存 string key                       → matMap[key] → MaterialHandle
                                       → meshComp.materialSlots[i] = handle
       ↑ save                           ↑
  ExportToDescription              属性卡修改时同步更新
  (读 m_entityDescs,               m_entityDescs 中的 string key
   不碰 handle)
```

**核心原则**：序列化路径不经过 handle。handle 只在运行时存在，导出时始终从 `m_entityDescs` 缓存的 string key 还原 JSON。

---

## 4. 动画与蒙皮的关系

**动画不感知 SubMesh**。

```
AnimationSystem（每帧更新骨骼矩阵）
     ↓  写入骨骼缓冲区
MeshComponent.lodMeshHandle（不变，同一网格）
     ↓
GPU 用同一套骨骼矩阵变换所有顶点
     ↓ 渲染时按 SubMesh 拆分
Draw(SubMesh 0, mat_body)   ← NPR 材质
Draw(SubMesh 1, mat_cloth)  ← PBR 材质
```

骨骼矩阵、顶点缓冲区、索引缓冲区在同一个 mesh 中，SubMesh 只是索引偏移。换材质不换绑定。

**蒙皮网格加载**：Step 0 中 DxMeshLoader 顺便读取 `DxMeshFlag_Skinned`，TriangleMesh 的 `flags` 字段记录此标记。蒙皮网格的 SubMesh 展开与静态网格完全相同，渲染器按 `SkinnedRenderer` 路径处理即可。

---

## 5. 实施步骤

| Step | 内容 | 关键文件 | 前置 | 状态 |
|:----:|:-----|:---------|:-----|:-----|
| **0** | **`.dxmesh` 格式扩展：Header 加 `subMeshCount`/`subMeshOffset`，文件末加 SubMesh 表** | `DxMeshFormat.h`、`DxMeshWriter.cpp`、`README.md` | 无 | ✅ 已落地 |
| **0a** | **TriangleMesh 加 `flags` + `subMeshes[]`，GeometryResourceManager 加 `GetSubMeshInfo`** | `TriangleMesh.h`、`GeometryResourceManager.h/.cpp` | Step 0 | ✅ 已落地 |
| **0b** | **DxMeshLoader / MeshLoadTask 读取 `flags` + SubMesh 表**（顺带修复 P1 蒙皮缺口） | `DxMeshLoader.cpp`、`MeshLoadTask.h/.cpp` | Step 0a | ✅ 已落地 |
| **1** | **MeshComponent 数组化 + 四端同步** | 见 §5.1 | Step 0b | ✅ 已落地（含 `MeshDesc.materials[]`、`ParseMesh`、`SceneConstructor`、`ExportToDescription` 四端） |
| **2** | **OpaqueRenderItemBuilder SubMesh 展开** | `OpaqueRenderItemBuilder.cpp` | Step 1 | ✅ 已落地（`GetSubMeshInfo` + 按槽取材质 + fallback `[0]`） |
| **3** | **MeshEditor 属性卡** | 新增 `MeshEditor.cpp`、注册到 `Editor::Initialize` | Step 1 | 📋 待做 |
| **4** | **蒙皮渲染器同样 SubMesh 展开** | `SkinnedRenderItemBuilder`（如存在） | Step 2 | 📋 待做（当前只消费 `materialSlots[0]`） |

### 5.2 观察验证（2026-08-02）

`Content/Scenes/async_test.scene.json` 已嵌入 KD-03 观察样本：

- 依赖：`"kd03": "Models/KD-03/kd-03.dxmesh"`（8 子网格，头文件 `subMeshCount=8` 已核对）
- 材质槽：实体 `KD03_Slots` 的 `mesh.materials[]` 8 个 key（`mat_kd03_0`~`mat_kd03_7`，内联材质，不同 baseColor 便于肉眼区分）
- 渲染路径：暂走 **Opaque 普通路径**（`"opaque": null`），忽略蒙皮绑定（KD-03 顶点前 44 字节布局与静态顶点一致，`OpaqueRenderer` 按 `mesh->vertexStride`=64 读取，可渲染绑定姿态）
- 预期：8 个子网格各以不同颜色渲染，验证 SubMesh 展开 + 槽位材质对应
- 注意：`SkinnedRenderItemBuilder` 已完成 SubMesh 展开（2026-08-02，统一语义），蒙皮角色槽位按子网格消费
- schema：`Schemas/scene.schema.json` MeshComponent 仅支持 `materials[]`（`material` 单字段已移除，`required` 降为 `["geometry"]`）

> **Step 4 (Builder 按 Shader 分发) 和 Step 5 (OpaqueTag 下沉) 暂缓**，当前无 NPR/PBR 多 Shader 路线需求，先保持现有 OpaqueTag 筛选模式。

### 5.3 已知问题：WindowFrameResources G-buffer getter 硬编码边界（2026-08-02 记录）

**问题**：`WindowFrameResources::GetGBufferResource/GetGBufferRTV/GetGBufferSRV` 的越界检查硬编码数量（本次从 `i >= 4` 改为 `i >= 5`），与 `m_gbuffer[5]` 数组及 `GetGBufferCount()` 存在**三处重复的魔法数字**。

**风险**：G-buffer 再次扩展 RT 数量（如加自定义数据通道）时，三处边界需同步修改，漏改一处即导致 `ResourceBarrier(NULL pointer)` 类崩溃（本次 emissive 通道扩展已实际踩坑：getter 仍返回 `i>=4`，`GetGBufferResource(4)` 返回 nullptr → D3D12 ERROR #520）。

**后续改进方向**（未排期）：
- getter 越界检查改为基于 `GetGBufferCount()`（单一事实来源），或使用 `std::size(m_gbuffer)` 推导，消除魔法数字
- 约束：`GetGBufferCount()` 与 `m_gbuffer` 数组本身也应从同一常量/枚举推导（如 `kGBufferCount`），避免只改一处
- 与数据驱动 PSO 的几何条件同理：RT 布局若走向数据化，边界也应成为描述的一部分（见 `RendererDataDriven.md` §2.6a）

### 5.1 Step 1 详细：四端同步

| 端 | 文件 | 改动 |
|:---|:-----|:------|
| **ECS 结构体** | `Engine/ECS/Core/Components/Render.h` | `materialHandle` → `std::vector<MaterialHandle> materialSlots`；去掉 `indexCount`/`startIndex`/`startVertex`（移到 SubMeshInfo） |
| **Desc 结构体** | `Engine/Asset/IO/Loader/SceneDescription.h` | `MeshDesc.material` → `std::vector<std::string> materials`；`from_json` 仅读 `"materials"` 数组（2026-08-02 移除旧 `"material"` 兼容分支） |
| **场景加载(Parse)** | `Engine/Asset/IO/Loader/SceneLoader.cpp` `ParseMesh` | 读 `"materials"` 数组（2026-08-02 移除向后兼容分支，固化为材质槽模式） |
| **ECS 创建** | `Engine/Scene/SceneConstructor.cpp` | `for key in materials[]: matMap → handle → materialSlots.push_back` |
| **ECS 导出** | `Editor/EditorLib/Scene/EditorSceneManager.cpp` `ExportToDescription` | 从 `m_entityDescs` 读 materials[]（不变，但属性卡改材质后需同步更新缓存） |

---

## 6. 待办索引

见 `Docs/todos/remaining_issues.md`：

- **#22** MeshComponent 材质槽数组化（§5 Step 1）
- **#23** MeshEditor 属性卡（§5 Step 3）
- **#24** SubMesh 级材质替换（§5 Step 0→2）

---

## 附录 A：冗余示例

一个角色网格有 5 个子网格，只有 3 种材质：

| SubMesh | 部位 | materials[] 值 |
|:-------:|:-----|:---------------|
| 0 | 身体 | `"mat_skin_npr"` |
| 1 | 头部 | `"mat_skin_npr"` ← 冗余 |
| 2 | 头发 | `"mat_hair_pbr"` |
| 3 | 左眼 | `"mat_eyes_pbr"` |
| 4 | 右眼 | `"mat_eyes_pbr"` ← 冗余 |

这是预期行为，与 Cocos/Unity 一致。用户按子网格视角操作，属性卡遍历显示 5 个槽，每个槽可独立替换。
