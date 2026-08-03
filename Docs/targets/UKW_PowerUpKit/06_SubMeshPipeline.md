# UKW 资产 SubMesh 合并管线

> 日期：2026-07-30（更新：2026-07-30，流程改为刚性绑定不解烘焙）
> 状态：📋 设计定案
> 关联：`Docs/architecture/rendering/SubMeshMaterialSlots.md`、`Docs/targets/UKW_PowerUpKit/02_RobotAndAnimation.md`、`AssetTool/`

---

## 1. 动机

UKW 的机器人模型以**部件拆分**方式存储——每个部件是一个独立的 `.x` 文件（Body.x、Head.x、arm1.x……），通过 `.hod` 文件定义骨架树和变换矩阵。

但引擎的渲染管线需要的是一份**完整的网格**（单一 `.dxmesh`），每个部件作为该网格的一个 SubMesh，共享同一份顶点/索引缓冲区。这样渲染管线才能：
- 通过 SubMesh 展开为每个部件分配不同材质
- 用骨骼矩阵统一驱动所有顶点（刚性绑定）

### 既有能力

| 模块 | 状态 | 说明 |
|:-----|:------|:------|
| **XFileParser** | ✅ assimp 解析 `.x` → `XFileMesh`（顶点/索引/材质） |
| **HODParser** | ✅ `.hod` → `HODData`（骨架树、各部件变换矩阵） |
| **DxMeshWriter** | ✅ 已支持 `subMeshes` 参数写入 SubMesh 表 |
| **SubMesh 展开** | ✅ Builder 层已支持 SubMesh × materialSlots 展开 |

### 缺口

已有能力之间缺少一个**组装环节**——将零散的 `.x` 部件按照 `.hod` 的定义，合并为一个带 SubMesh 表的 `.dxmesh`。

---

## 2. 合并管线总览

```
Robo/KD-03/
├── Robo.hod                    HODParser
│     └── 部件列表 + 变换矩阵 ──────┐
│                                  ▼
├── Body.x          ─┐      XFileParser  × N
├── Head.x           ├──── 逐个解析 .x 文件
├── arm1.x           │         ↓
├── arm1_r.x         │     XFileMesh[0..N]
├── leg1.x           │    (独立顶点/索引, 局部坐标)
├── leg2.x           │         ↓
├── leg3.x           │     【合并步骤】
└── ...             ─┘         ↓
                          ┌────┴────┐
                          │         │
                    同骨格合并 → LR 交换
                    (assimp 拆分     (交换 _r 与
                     网格重新合并)     左部件的
                                        boneIndex)
                          │         │
                          └────┬────┘
                               │
                         骨骼矩阵修正
                    (Body_d偏移 → 层级累乘 → Ry(180°))
                          │         │
                    ┌─────┴─────────┴─────┐
                    │                     │
              顶点不解烘焙              hod.json
              (局部坐标 +              (骨架树)
               DxMeshSkinnedVertex)
                    │                     │
                    └─────┬───────────────┘
                          ▼
                    .dxmesh 文件
              (单一 VB/IB + N 个 SubMesh
               + 顶点 boneIndex/weight)

                    scene.json
              (网格引用 + 材质列表 + 骨架引用)
```

### 2.1 合并的实质

合并不是"把多个网格焊接成一个连续表面"，而是**将多个独立的顶点/索引缓冲区拼接到同一个缓冲区**中：

```
.B.x 顶点[0..499]  索引[0..1499]   →  合并后顶点[0..499]    索引[0..1499]
Head.x 顶点[0..99]  索引[0..299]    →  合并后顶点[500..599]  索引[1500..1799]
arm1.x 顶点[0..199] 索引[0..599]   →  合并后顶点[600..799]  索引[1800..2399]
...

SubMesh 表:
  [{startIndex=0,     indexCount=1500, startVertex=0},     // Body
   {startIndex=1500,  indexCount=300,  startVertex=500},    // Head
   {startIndex=1800,  indexCount=600,  startVertex=600},    // arm1
   ...]
```

每个部件网格的**索引需要 rebase**（偏移量 = 之前所有部件的顶点数之和），而顶点数据直接追加。

### 2.2 蒙皮绑定：DxMeshSkinnedVertex 刚性绑定

UKW 的每个部件（每个 `.x` 文件）对应 HOD 中的一根骨骼，该部件的所有顶点由这根骨骼驱动。这是**刚性绑定（rigid skinning）**，而非传统 4-weight 蒙皮。

合并时使用 `DxMeshSkinnedVertex` 格式：

```
每个顶点:
  float3  position      ← 局部坐标（不变换到世界空间）
  float3  normal        ← 局部法线
  float2  texC          ← UV
  float4  boneWeights   = {1.0f, 0, 0, 0}   // 刚性绑定，1 根骨骼
  uint8_t boneIndices[4] = {partBoneIndex, 0, 0, 0}  // 所属骨骼索引
```

- 顶点**不烘焙到世界空间**，保持 `.x` 文件中的局部坐标
- 运行时由 `boneWorld[boneIndex]` 驱动顶点变换
- 所有顶点格式统一为 64 字节 `DxMeshSkinnedVertex`，与引擎现有蒙皮格式一致
- `DxMeshHeader.flags` 设为 `DxMeshFlag_Skinned`

### 2.3 材质与 COLORSET

每个部件 `.x` 文件内部自带一个 `Material` 块（漫反射颜色、光泽度、纹理名）。合并时：

- 每个部件的材质信息提取为 `MaterialDesc`，放入场景的 `materials[]` 字典
- 合并后的 `.dxmesh` 的 `materials` 数组的 key 按部件命名：`"KD-03_Body"`、`"KD-03_Head"` 等
- COLORSET 指令（`COLORSET(配色, 材质索引, a,r,g,b)`）可以在场景加载后作为 MaterialOverride 应用

---

## 3. AssetToolGUI 按钮设计

### 3.1 "导入机体"按钮

在 AssetToolGUI 中添加一个"导入机体"按钮，执行以下操作：

```
用户操作:
  在文件浏览器中选择 Robo/KD-03/Robo.hod

程序执行:
  1. 解析 .hod → 得到部件列表（Body.x, Head.x, arm1.x, ...）
  2. 筛选出可渲染部件（排除 Hit.x/HitUp.x/Weapon_point*.x 等辅助网格）
  3. 对每个部件 .x 文件:
     a. XFileParser 解析 → XFileMesh
     b. 提取材质信息 → MaterialDesc
  4. 合并所有 XFileMesh 为单一网格 + SubMesh 表
  5. 调用 DxMeshWriter::Write(..., subMeshes, subMeshCount) 输出 .dxmesh
  6. 输出 hod.json（骨架数据）
  7. 输出 scene.json 片段（网格引用 + 材质列表 + 骨架引用）
```

### 3.2 可筛选的部件类型

并非 `.hod` 中所有部件都需要合并为 SubMesh。按类型区分：

| 类型 | 举例 | 合并方式 |
|:-----|:------|:---------|
| **身体部件** | Body.x, Head.x, arm1.x, leg1.x, Hand.x | → SubMesh |
| **武器/装备** | gun.x, sword.x, Shield.x, Missile.x | → SubMesh（可选，或单独加载） |
| **碰撞体** | Hit.x, HitUp.x | 排除，不合并 |
| **挂点** | Weapon_point*.x, root_Fannel.x | 排除，不合并 |
| **根节点** | root.x | 排除（无几何体） |

筛选规则硬编码为：文件名以 `Hit`、`Weapon_point`、`root` 开头的排除，其余合并。

### 3.3 输出文件结构

```
Content/Models/KD-03/
├── KD-03.dxmesh          ← 合并后的网格（含 SubMesh 表）
├── KD-03.hod.json        ← 骨架数据（来自 HODParser）
├── KD-03.scene.json      ← 场景片段（网格引用 + 材质列表）
└── textures/             ← 贴图（从 .x 引用的 .dds/.bmp 转换）
    ├── body.dds
    ├── head.dds
    └── ...
```

---

## 4. 数据结构

### 4.1 合并网格的 DXMesh 布局

```
Header (128B)
├── vertexCount     = Body.verts + Head.verts + ...
├── indexCount      = Body.indices + Head.indices + ...
├── vertexStride    = 64 (DxMeshSkinnedVertex，统一蒙皮格式)
├── flags           = DxMeshFlag_Skinned (刚性绑定也是 skinned)
├── lodCount        = 1
├── subMeshCount    = 部件数量（筛选后）
└── subMeshOffset   = 指向 SubMesh 表

Vertex Data (DxMeshSkinnedVertex，所有部件顶点连续排列)
  ┌─ position[3]     // 局部坐标，不变换
  ├─ tangentU[3]     // 默认 {1,0,0}
  ├─ normal[3]       // 局部法线
  ├─ texC[2]         // UV
  ├─ boneWeights[4]  // {1.0f, 0, 0, 0}
  └─ boneIndices[4]  // {partBoneIndex, 0, 0, 0}
  total = 64B/顶点

Index Data (所有部件的索引数据，经 rebase 后)
LOD Table (单 LOD，覆盖整个网格)
SubMesh Table (N 个 DxMeshSubMesh，按 .hod 中部件顺序)
  ├── DxMeshSubMesh[0] = Body  → {indexOffset=0,  indexCount=N,  vertexOffset=0}
  ├── DxMeshSubMesh[1] = Head  → {indexOffset=N,   indexCount=M,  vertexOffset=V}
  └── ...
```

### 4.2 hod.json（骨架数据）

HODParser 已有输出格式，包含部件名/父子关系/TRS。无需额外改动，引擎侧需要补充一个 `SkeletonManager` 加载该 JSON 的接口（当前为骨架实现）。

### 4.3 scene.json 片段

```json
{
    "entities": [
        {
            "name": "KD-03",
            "components": {
                "transform": { "position": [0,0,0], "rotation": [0,0,0,1], "scale": [1,1,1] },
                "mesh": {
                    "geometry": "Content/Models/KD-03/KD-03.dxmesh",
                    "materials": [
                        "KD-03_Body",
                        "KD-03_Head",
                        "KD-03_arm1",
                        "KD-03_arm1_r",
                        "KD-03_leg1",
                        "KD-03_leg1_r",
                        "KD-03_leg2",
                        "KD-03_leg2_r",
                        "KD-03_leg3",
                        "KD-03_leg3_r",
                        "KD-03_Hand",
                        "KD-03_Hand_r"
                    ]
                },
                "skinned": {
                    "skeleton": "Content/Models/KD-03/KD-03.hod.json"
                }
            }
        }
    ],
    "dependencies": {
        "KD-03_Body": "Content/Models/KD-03/textures/body.dds",
        "KD-03_Head": "Content/Models/KD-03/textures/head.dds"
    }
}
```

---

## 5. 实施步骤（2026-07-30 更新；2026-08-01 标注退役）

> ⚠️ **2026-08-01 路线定案**：本表的 A 系列（importrobot / .x 直接拼接）已**退役不调用**，仅作参考实现。引擎资产唯一来源是 Blender 优化后的最终 FBX（见 `07_EngineAssetPipeline.md`，fbxs2dxmesh 立项 P1）。下表保留历史状态供参考。

| Step | 内容 | 关键文件 | 状态 |
|:----:|:------|:---------|:------|
| **A** | **AssetTool: 实现合并命令** — 解析 .hod → 合并 .x 部件 → 输出 .dxmesh（DxMeshSkinnedVertex + SubMesh 表）+ hod.json + scene.json | `AssetTool/AssetToolGUI.cpp` | ✅ 已实现（GUI，已退役） |
| **A1** | **变换修正** — Body_d 偏移 + 层级累乘 + Ry(180°) 翻转 | `AssetTool/AssetToolGUI.cpp` | ✅ 已实现（规则被 fbxs2dxmesh 复用） |
| **A2** | **LR 交换** — 复选框控制 _r 骨骼交换 | `AssetTool/AssetToolGUI.cpp` | ✅ 已实现（FBX 路径不再需要） |
| **A3** | **切换到 DxMeshSkinnedVertex** — 顶点不解烘焙，刚性绑定 boneIndex/weight | `AssetTool/AssetToolGUI.cpp` | ✅ 已实现（skinned 输出，A3 标记已过时；FBX 路径直接读权重） |
| **B** | **AssetToolGUI: 添加"导入机体"按钮** | `AssetTool/AssetToolGUI.cpp` | ✅ 已实现（已退役） |
| **C** | **引擎侧: SkeletonManager 加载 hod.json** — 从 JSON 加载骨架数据并注册 | `SkeletonManager.h/.cpp` | ❌ 未实现（改为从 `.bone` 加载，见待办 U2） |
| **D** | **引擎侧: SkinnedComponent 接入** — 场景加载时创建 SkinnedComponent | `SceneConstructor.cpp` | ⚠️ 框架已有（见待办 U3） |
| **E** | **ANI 动画解析** | `AssetTool/Core/ANIParser.cpp` | ✅ 已实现（2026-07-31，1.008 + PUK 双格式，标记法 + 母版驱动） |

> 注意：Step A 的 CLI 版本（`main.cpp` 的 `CommandImportRobot`）不含 A1/A2 的修正步骤，功能落后于 GUI 版本。未来应考虑目录重构：`AssetTool/Core/` 公共逻辑、`AssetTool/CLI/`、`AssetTool/GUI/`。

---

## 6. 遗留问题与后续

### 6.1 场景构建（延迟）

地图场景的自动构建（MPD → scene.json）已放弃。原因：

- **天空半球**：旧资产使用 `Sky.x`（半球网格）+ 雾色模拟天空，非立方体贴图模式。引擎当前 `SkyRenderer` 基于立方体贴图，需在编辑器端增加"半球天空 → 颜色/雾"的兼容映射
- **逆向缺口**：MPD 坐标段格式各地图不一致，无法通用解析（详见 `05_MPD_Format_Analysis.md` §十二结论）

**当前策略**：编辑器手动布景。AssetTool 的 MPD 解析作为参考信息源，不自动生成场景。

### 6.2 资产缺失容错

AssetTool 转换时若缺少部分字段（如材质纹理引用、COLORSET 参数、受损变体骨骼），不应阻塞整体转换。策略：

- 字段缺失 → 使用默认值/空值
- 文件缺失（如 Body_d.x 受损变体）→ 跳过单个部件，继续处理其余
- 场景 JSON 中的缺字段由编辑器端校验和修正

### 6.3 动画解析（暂缓）

`.ani` 文件解析（文件名头 + HOD 块序列 + Tail 状态机脚本）当前不处理。骨架数据（.hod.json）已足够支持静态展示和蒙皮网格的 SubMesh 验证。

### 6.4 后续可能的资产管线扩展

| 项目 | 时机 | 说明 |
|:-----|:------|:------|
| COLORSET → MaterialOverride | 编辑器材质系统就绪后 | COLORSET 指令定义每部件的配色参数，可作为材质覆盖层 |
| 受损变体切换 | 后期 | Body_d.x 等受损模型可在 HP 低于阈值时切换 |
| 武器独立加载 | 后期 | gun/sword/shield 等作为单独实体挂载，而非合并到主体网格 |
