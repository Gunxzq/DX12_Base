# 骨骼网格资产管线

> 日期：2026-07-31
> 状态：方向文档（设计定案）
> 关联：`SubMeshMaterialSlots.md`、`RelationshipModel.md`、`CharacterAsset.md`、`AssetTool_ExportPipeline_Snapshot_20260731.md`、`UKW_PowerUpKit/06_SubMeshPipeline.md`

---

## 一、动机

项目中涉及多个需要统一表述的概念：骨骼（Skeleton Bone）、子网格（SubMesh）、材质槽（Material Slot）、挂点（Socket）。这些概念在 UKW 机器人中以"每骨骼一部件"的 1:1 特例出现，但现代角色/载具资产中它们是多对多的正交关系。

同时，工具链（Blender → AssetTool → 引擎运行时）需要一份端到端的说明，明确资产格式约定和挂载系统的工作方式。

---

## 二、核心概念与正交关系

### 三条独立轴

```
骨骼树 (Skeleton)              子网格表 (SubMesh)             材质槽 (Material Slots)
───────────────               ──────────────────             ──────────────────────
动画驱动的层级节点             顶点/索引的连续切片             每子网格一个 MaterialHandle
root                          [0] Body (0..499顶点)          [0] mat_body_npr
├─ Spine                      [1] Head (500..599)           [1] mat_head_npr
│   ├─ Head     ← 有几何体    [2] arm_L (600..799)          [2] mat_arm_pbr
│   ├─ arm_L    ← 有几何体    [3] arm_R (800..999)           [3] mat_arm_pbr
│   ├─ arm_R    ← 有几何体    [4] hand_L (1000..1099)        [4] mat_hand_npr
│   ├─ hand_L   ← 有几何体    [5] hand_R (1100..1199)        [5] mat_hand_npr
│   ├─ hand_R   ← 有几何体    ...                            ...
│   ├─ Weapon_point ← 纯挂点  N=29                           N=29
│   └─ Muzzle_01   ← 纯挂点
└─ leg_L ...
```

**三者没有硬性的 N:N 约束**，在运行时是完全正交的：

| 组合 | 骨骼:子网格 | 说明 | 示例 |
|:-----|:-----------|:------|:------|
| **刚性绑定 1:1** | 每根骨骼恰好驱动一个 SubMesh | UKW 机器人，每部件一根骨骼 | `Body` 骨骼 → SubMesh[0] → Body 几何体 |
| **蒙皮 N:1** | 多根骨骼混合驱动一个 SubMesh | 标准角色蒙皮（4-weight） | 整身一个 SubMesh，由 ~20 根骨骼驱动 |
| **骨骼 1:0（挂点）** | 骨骼无几何体 | 纯挂点，供武器/特效附着 | `Weapon_point_R`、`Muzzle_01` |
| **子网格无骨骼** | 几何体不受蒙皮驱动 | 静态装饰、碰撞体轮廓 | 建筑上的独立部件 |

### 数据结构映射

```
.dxmesh 文件 → TriangleMesh
  ├─ vertexBuffer / indexBuffer（全部顶点/索引连续存储）
  ├─ subMeshes[]（索引切片：startIndex/indexCount/startVertex）
  └─ flags → DxMeshFlag_Skinned（顶点带 boneIndices/boneWeights）

ECS MeshComponent
  └─ materialSlots[]（长度 = subMeshes.size()，索引一一对应）

.bone / SkeletonManager
  └─ 骨骼树（命名、父子关系、rest pose 矩阵） → 不依赖子网格

> **骨架资产定位**：正式骨架资产为 `.bone`（`AssetType::Skeleton`），由 AssetTool 从 HOD 解析导出。`hod.json` 只是 AssetTool 用于**预览 HOD 解析结果**的中间格式，**不进入资产体系**（详见 `Docs/architecture/CharacterAsset.md` §四）。

ECS RelationshipComponent (kind=socket)
  └─ socketName（挂点名称 = 骨骼名） → 指向骨骼树中的节点
```

### 实现状态总览

三种映射模式在各层的支持状况：

| 模式 | 文件格式 | 加载器 | Skeleton Mgr | SkinnedRdr Builder | GPU 骨骼缓冲 | 挂点 System |
|:-----|:---------|:-------|:-------------|:------------------|:------------|:------------|
| **静态网格（无骨骼）** | ✅ `DxMeshStaticVertex` | ✅ | n/a | n/a | n/a | n/a |
| **刚性绑定 1:1** | ✅ `DxMeshSkinnedVertex` | ✅ 读 flags+subMeshes | ⚠️ 无 `.bone` 加载 | ✅ 已实现 | ❌ 未接通 | n/a |
| **蒙皮 N:1** | ✅ 同一格式，boneWeights 可分配 | ✅ 同上 | ⚠️ 同上 | ✅ 已实现 | ❌ 未接通 | n/a |
| **纯挂点 1:0** | n/a | n/a | ✅ 骨架数据可加载挂点骨骼 | n/a | n/a | ⚠️ ECS 组件就绪，无消费 System |

**各模块详细状态：**

| 模块 | 文件 | 状态 | 说明 |
|:-----|:------|:------|:------|
| `DxMeshFormat.h` | `Engine/Asset/Definitions/Mesh/` | ✅ | `DxMeshStaticVertex`(44B) + `DxMeshSkinnedVertex`(64B, 含 `boneWeights[4]`/`boneIndices[4]`)、`DxMeshSubMesh` 动态表、`DxMeshFlag_Skinned` 标记 |
| `DxMeshLoader.cpp` | `Engine/Asset/IO/Loader/` | ✅ | 读 `flags` → `TriangleMesh.flags`，读 `subMeshes[]` → `TriangleMesh.subMeshes` |
| `MeshLoadTask.h` | `Engine/Background/` | ✅ | 异步路径同样正确读取 flags + subMeshes |
| `SkeletonManager` | `Engine/Resource/Manager/` | ⚠️ 缺 `.bone` 加载 | `SkeletonData`（`BoneHierarchy[]`+`BoneOffsets[]`+`BoneNames[]`）能表达 `.bone` 内容（HOD 解析导出），现有 `LoadFromM3d()` 只走 M3D 格式，缺少从 JSON 加载的路径。`ComputeFinalTransforms` 可直接复用（走到 rest pose 即骨骼世界矩阵） |
| `SkinnedRenderItemBuilder` | `Engine/Renderer/RenderItemBuilder/` | ✅ | 已实现，构造函数接收 `SkeletonManager*` |
| `SkinnedRenderer` | `Engine/Renderer/Pipeline/` | ⚠️ 缺骨骼缓冲 | G-buffer PSO + 根签名已创建，`DrawGBuffer()` 存在，但缺少每帧将 CPU 骨骼矩阵上传到 GPU BoneBuffer 的管道 |
| `RelationshipComponent` | `Engine/ECS/Core/Components/` | ✅ | `RelationshipKind::Socket` + `SocketAttachmentComponent` 均已实现，SceneConstructor 从 JSON 加载创建 |
| `WeaponAttachmentSystem` | 暂无 | ❌ | 尚未实现，socket 数据在 ECS 中但无人消费 |

### 当前运行时缺口

**1. SkeletonManager 缺少 `.bone` 加载**

```cpp
// ✅ 已有
SkeletonHandle LoadFromM3d(const std::string &filepath);

// ❌ 缺少
SkeletonHandle LoadFromJSON(const std::string &bonePath);  // ~200 行，数据映射已有（hod.json 解析结果 → .bone）
```

**2. GPU 骨骼缓冲未接通**

每帧需要完成：`SkeletonManager::ComputeFinalTransforms()` → 拷贝结果到 GPU upload buffer → `SetGraphicsRootShaderResourceView` 绑定到 SkinnedRenderer 的 BoneBuffer SRV 槽位。当前这段管线是断的。

**3. 挂点消费 System 不存在**

`RelationshipComponent{kind:socket, socketName}` 数据已正确加载到 ECS 中，但没有 System 每帧读取骨骼矩阵并更新挂载实体的 TransformComponent。

---

## 三、dxmesh 子网格容量

### 格式层（无硬上限）

`DxMeshHeader.subMeshCount` 是 `uint32_t`，写入/读取均使用动态向量：

```cpp
// DxMeshWriter.cpp
ofs.write(subMeshes, sizeof(DxMeshSubMesh) * subMeshCount);

// DxMeshLoader.cpp / MeshLoadTask.h
outMesh.subMeshes.reserve(header->subMeshCount);
for (uint32_t i = 0; i < header->subMeshCount; ++i) { ... }
```

`DxMeshSubMesh` 仅 12 字节（3 × `uint32_t`）。按典型场景估算：

| 场景 | 子网格数 | 表大小 | 是否可行 |
|:-----|:---------|:-------|:---------|
| UKW 机器人 | ~29 | 348 B | ✅ |
| Genshin 级角色 | 50-150 | 0.6-1.8 KB | ✅ |
| 机甲换装 | 100-300 | 1.2-3.6 KB | ✅ |
| 极端材质拆分 | 1000+ | 12 KB+ | ✅ |

### 实际约束（材质数而非子网格数）

子网格数量不是渲染性能的直接约束。**性能瓶颈在材质切换次数（PSO 换入换出）**，而非子网格数。SubMesh 展开后的 DrawCall 数量级在 100-300 对现代 GPU 是轻松的。

**材质分组减少子网格数**：Blender 中多个部件如果共用同一材质，导出时会被合并为同一个 SubMesh（见 §四）。这使得最终子网格数≈材质种类数，而非部件数。

---

## 四、Blender → AssetTool → 引擎管线

### 整体流程

```
┌────────────┐    ┌──────────────┐    ┌───────────────────┐    ┌────────────┐
│  Blender   │ →  │  AssetTool   │ →  │  引擎异步加载     │ →  │  运行时    │
│            │    │  (CLI/GUI)   │    │  MeshLoadTask     │    │  ECS 装配  │
│ .x / .fbx  │    │  RobotMerger │    │  ↓                │    │            │
│ 部件零散   │    │  ↓           │    │  TriangleMesh     │    │  MeshComp  │
│            │    │  .dxmesh     │    │  subMeshes[]      │    │  material- │
│ HOD        │    │  + .bone     │    │  flags(skinned)   │    │  Slots[]   │
│ 骨骼树     │    │  + .character│    │                   │    │            │
└────────────┘    └──────────────┘    └───────────────────┘    └────────────┘
```

> 注：AssetTool 的 HOD 解析中间结果（`hod.json`）仅供预览，正式骨架资产导出为 `.bone`（详见 §二 骨架资产定位）。

### Blender 工作流约定

Blender 中每个**材质槽（Material Slot）**对应导出一个 SubMesh。控制子网格数量的方法：

- **不需要 1:1 部件映射**：可以把多个部件指定同一材质，Blender 导出时它们被合并为一个 SubMesh
- **材质命名规范**：材质名称作为 scene.json 中 `materials[]` 的 key，AssetTool 直接沿用
- **骨骼命名规范**：挂点骨骼建议统一前缀（如 `Sock_hand_R`、`Fx_Muzzle`），便于引擎侧做 socket 识别

### AssetTool 导出（RobotMerger）

| 输出 | 内容 | 用途 |
|:-----|:------|:------|
| `KD-03.dxmesh` | 合并后的顶点/索引 + SubMesh 表 + 蒙皮标记 | 渲染用网格 |
| `KD-03.bone` | 骨骼树 + 每骨骼 rest pose 矩阵（HOD 解析导出） | SkeletonManager 加载 |
| `KD-03.character` | 角色复合资产：引用 dxmesh + bone + 材质槽 + 动画剪辑 | CharacterLoader / SceneConstructor |

当前 AssetTool 输出的是**原子资产**（单一 `dxmesh` + 独立 `.bone`）。角色复合资产 `.character`（含动画剪辑、材质槽）为后续方向，详见 `Docs/architecture/CharacterAsset.md`。

---

## 五、挂点系统（Socket）

### 武器与特效发射口：同一机制

武器和特效发射口本质上都是**挂载到某根骨骼的独立实体**，引擎层面不做区分：

```
骨骼 "hand_R"（挂点，有几何体或无几何体均可）
  ├─ socket → 武器实体（MeshComponent + 碰撞体 → 可掉落、可拾取）
  └─ socket → 特效实体（ParticleEmitterComponent + 生命周期 → 播放完自销毁）
```

运行时由不同 Gameplay System 消费：

```cpp
// WeaponAttachmentSystem —— 处理武器挂载
if (rel.kind == Socket && socketName == "hand_R")
    weapon.transform = anim.GetBoneWorldMatrix(target, "hand_R");

// EffectAttachmentSystem —— 处理特效发射口
if (rel.kind == Socket && socketName starts_with "Fx_")
    spawnPos = anim.GetBoneWorldMatrix(target, socketName);
```

### 场景 JSON 中的挂点声明

武器、特效等挂载物在 scene.json 中以独立实体存在，通过 `relationships` 引用目标骨骼：

```json
{
    "name": "weapon_sword",
    "persistentId": "c3d4e5f67890a1b2",
    "components": {
        "transform": { "position": [0, 0, 0] },
        "mesh": {
            "geometry": "sword.dxmesh",
            "materials": ["weapon_mat"]
        }
    },
    "relationships": [
        { "kind": "socket", "targetId": "a1b2c3d4e5f67890", "socketName": "hand_R" }
    ]
}
```

### 挂点与子网格的独立关系

挂点（socket）只依赖骨骼命名，不依赖子网格是否存在：

```
骨骼 hand_R:
  ├─ 子网格 SubMesh[5] → hand_R 几何体（可渲染，也可不渲染）
  └─ 挂点 socket       → 武器位置同步（与子网格无关）

# 即使 hand_R 没有几何体（骨骼 1:0），武器仍然能挂在 hand_R 上
```

---

## 六、映射模式示例

### UKW 机器人（刚性绑定 1:1）

```
骨骼树              子网格              材质槽
Body       →   SubMesh[0] Body    →  [0] mat_body
Head       →   SubMesh[1] Head    →  [1] mat_head
arm_L      →   SubMesh[2] arm_L   →  [2] mat_arm
hand_R     →   SubMesh[3] hand_R  →  [3] mat_hand
Weapon_point →  无子网格（纯挂点）    —    ← 武器 socket 挂载到这里
```

### 标准角色蒙皮（N:1）

```
骨骼树              子网格              材质槽
Spine        ─┐
  ├─ Head    ─┤
  ├─ arm_L   ─┤
  ├─ arm_R   ─┤→ SubMesh[0] 全身  →  [0] mat_body_npr
  ├─ leg_L   ─┤                   →  [1] mat_hair_pbr
  └─ leg_R   ─┘
                             子网格仅 1 个，但由 ~20 根骨骼混合蒙皮
                             材质槽 2 个（NPR 身体 + PBR 头发）不依赖子网格数
```

### 车辆载具（混合模式）

```
骨骼树              子网格              材质槽
HullBody     →   SubMesh[0] 车体   →  [0] mat_body
TurretBase   →   SubMesh[1] 炮塔基座 →  [1] mat_armor
GunBarrel    →   SubMesh[2] 炮管    →  [2] mat_armor   ← 与 [1] 同材质，Blender 可合并
Wheel_FL     →   SubMesh[3] 轮子    →  [3] mat_rubber
Wheel_FR     →   SubMesh[4] 轮子    →  [3] mat_rubber   ← 同材质
Muzzle       →  无子网格（纯挂点）     —    ← 开火特效附着
```

---

## 七、实施路线

| 阶段 | 内容 | 依赖 |
|:----|:------|:------|
| **A** | dxmesh 格式扩展：Header + SubMesh 表 + `flags`（skinned） | ✅ 已完成 |
| **B** | TriangleMesh 加 `subMeshes[]` + `flags`，DxMeshLoader/MeshLoadTask 读取 | ✅ 已实现 |
| **C** | **MeshComponent 材质槽数组化**（`materialHandle` → `materialSlots[]`） | 当前工作 |
| **D** | OpaqueRenderItemBuilder SubMesh 展开 | C |
| **E** | **SkeletonManager** 加载 `.bone`，运行时提供 `GetBoneWorldMatrix(name)` | 待启动 |
| **F** | **WeaponAttachmentSystem + 挂点同步**（RelationshipComponent socket 消费者） | E |
| **G** | 编辑器材质槽属性卡（MeshEditor） | C + 现有 ComponentEditorRegistry |

**C 和 E 无依赖关系，可并行推进。**

---

## 八、相关文档

- `Docs/architecture/SubMeshMaterialSlots.md` — 材质槽系统详细设计（Step 0→3）
- `Docs/architecture/RelationshipModel.md` — 实体关系模型（含 socket 挂载）
- `Docs/targets/UKW_PowerUpKit/06_SubMeshPipeline.md` — UKW 部件合并管线
- `Docs/snapshots/AssetTool_ExportPipeline_Snapshot_20260731.md` — AssetTool 导出状态
- `Docs/architecture/SkinnedAnimation.md` — 蒙皮动画系统设计
