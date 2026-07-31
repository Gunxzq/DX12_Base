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

**向后兼容规则**：
- 旧 JSON `"material": "mat_body"` 等效于 `"materials": ["mat_body"]`
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
    uint32_t startIndex;
    uint32_t indexCount;
    int32_t startVertex;
};

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
    if subMeshes 为空:
      旧路径: 用 materialSlots[0] (或 materialHandle 兼容)
      产生 1 RenderItem (整个 mesh)
    else:
      for i = 0 .. subMeshes.size():
        matHandle = (i < materialSlots.size() && materialSlots[i].IsValid())
                    ? materialSlots[i] : materialSlots[0] (fallback)
        materialIdx = matMgr->GetGPUIndex(matHandle)
        BatchKey{geoHandle, materialIdx, subMeshes[i].startIndex, ...}
        → 1 RenderItem (per SubMesh)
```

**合批规则不变**：同 `{geoHandle + materialIdx + startIndex/indexCount/startVertex}` 合并为一次 Draw Call。相同材质的不同 SubMesh（如 body_npr 在 submesh 0 和 1 出现两次）由于 `startIndex` 不同不会错误合并，但同材质 + 同偏移的跨实体 SubMesh 可合批。

### 2.6 渲染器层（零改动）

所有渲染器（OpaqueRenderer/ShadowRenderer/SkinnedRenderer）已经通过 `item.startIndex` / `item.indexCount` / `item.startVertex` 绘制。Builder 展开后每个 RenderItem 的这三个值就是 SubMesh 级别的，渲染器照常执行：

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

| Step | 内容 | 关键文件 | 前置 |
|:----:|:-----|:---------|:-----|
| **0** | **`.dxmesh` 格式扩展：Header 加 `subMeshCount`/`subMeshOffset`，文件末加 SubMesh 表** | `DxMeshFormat.h`、`DxMeshWriter.cpp`、`README.md` | 无 |
| **0a** | **TriangleMesh 加 `flags` + `subMeshes[]`，GeometryResourceManager 加 `GetSubMeshInfo`** | `TriangleMesh.h`、`GeometryResourceManager.h/.cpp` | Step 0 |
| **0b** | **DxMeshLoader / MeshLoadTask 读取 `flags` + SubMesh 表**（顺带修复 P1 蒙皮缺口） | `DxMeshLoader.cpp`、`MeshLoadTask.h/.cpp` | Step 0a |
| **1** | **MeshComponent 数组化 + 四端同步** | 见 §5.1 | Step 0b |
| **2** | **OpaqueRenderItemBuilder SubMesh 展开** | `OpaqueRenderItemBuilder.cpp` | Step 1 |
| **3** | **MeshEditor 属性卡** | 新增 `MeshEditor.cpp`、注册到 `Editor::Initialize` | Step 1 |
| **4** | **蒙皮渲染器同样 SubMesh 展开** | `SkinnedRenderItemBuilder`（如存在） | Step 2 |

> **Step 4 (Builder 按 Shader 分发) 和 Step 5 (OpaqueTag 下沉) 暂缓**，当前无 NPR/PBR 多 Shader 路线需求，先保持现有 OpaqueTag 筛选模式。

### 5.1 Step 1 详细：四端同步

| 端 | 文件 | 改动 |
|:---|:-----|:------|
| **ECS 结构体** | `Engine/ECS/Core/Components/Render.h` | `materialHandle` → `std::vector<MaterialHandle> materialSlots`；去掉 `indexCount`/`startIndex`/`startVertex`（移到 SubMeshInfo） |
| **Desc 结构体** | `Engine/Asset/IO/Loader/SceneDescription.h` | `MeshDesc.material` → `std::vector<std::string> materials`；`from_json` 兼容旧 `"material"` → `["material"]` |
| **场景加载(Parse)** | `Engine/Asset/IO/Loader/SceneLoader.cpp` `ParseMesh` | 读 `"materials"` 数组，向后兼容 |
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
