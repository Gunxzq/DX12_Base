# UKW 引擎侧资产管线推进方向

> 日期：2026-08-01（第二次定案：FBX 唯一来源；当晚推进：fbxs2dxmesh 已实现并验证通过、动画资产/视口文档新增）
> 状态：✅ 路线定案（FBX 为主，importrobot 退役）；阶段 1 完成（fbxs2dxmesh 转换 kd-03 成功：8 子网格/3045 顶点/15 骨骼）
> 关联：`06_SubMeshPipeline.md`（SubMesh 合并管线）、`AssetTool/Core/RobotMerger.cpp`（importrobot 实现，已退役）、`AssetTool/Core/RobotMergerFBX.cpp`（ani2anim 实现，保留）、`Docs/architecture/scene/RelationshipModel.md`（socket 挂载模式）、`Docs/architecture/animation/AnimationAsset.md`（.anim 资产规范，新）、`Docs/architecture/animation/AnimationViewport.md`（动画视口，新）
> 范围：引擎侧蒙皮/骨架管线（A3 skinned 顶点已就绪、C SkeletonManager、D SkinnedComponent），DCC 侧已定案（骨骼视觉断开接受，仅二进制 FBX 导出有效）

---

## 〇、分工定案（2026-08-01 用户定案）

| 环节 | 职责 | 说明 |
|:-----|:-----|:-----|
| **Blender** | 生产/编辑资产（网格、材质、骨骼、动画） | 资产唯一来源是 Blender 优化后的最终 FBX（含动画 AnimStack） |
| **AssetTool 转换器** | FBX → 游戏资产 | `fbxs2dxmesh`（网格/骨骼/材质/场景）+ `anim2clip`（动画 → .anim，待实施） |
| **游戏引擎** | 运行时调整（加载/播放/渲染/预览） | **不做建模/动画编辑**——不是建模软件 |
| **编辑器动画视口** | 查看角色与动画（播放/调帧/骨骼调试） | 角色**不进主 viewport**；`SceneConstructor` 生成器**暂不改**（NPC 场景化走 .character，P2） |

---

## 一、DCC 侧定案回顾（2026-08-01）

- **骨骼视觉断开是 UKW 数据固有**：部件变换矩阵语义（非关节链几何），多分支父骨骼无法全连；任何 head/tail 调整会破坏蒙皮/动画配对 → **接受现状**
- **唯一有效改动**：FBX 导出改**二进制格式**（Blender 5.2 不支持 ASCII），导入勾选 Force Connect Children 单链可连接
- 用户已在 Blender 中**合并子网格后重新导出**：`C:\Users\32199\Desktop\kd-03.fbx`（1 Armature / 20 骨骼 / 38 mesh / 38 材质，verifyfbx 实测：38 mesh 按材质拆分、每 mesh 刚性绑定 1 根骨骼）

## 二、路线定案（2026-08-01 第二次定案）：FBX 为引擎资产唯一来源

**引擎侧原始资产的唯一确认来源 = Blender 优化后的最终 FBX**。资产转换工具不再直接从原始 UKW 资产（.hod/.x）产出引擎资产。

```
原始 UKW（.hod/.x/ANI）
  → AssetTool ani2anim（ANI → 初始 FBX，进 Blender 的桥，保留）
  → Blender 优化（修模、合并子网格/材质、骨骼整理）→ 最终 FBX  ✅ 唯一可信源
  → fbxs2dxmesh（新增转换器，P1 立项）→ .dxmesh + .bone + .mat + scene.json → 引擎
```

### 关键决策点

1. **importrobot（路径 A，.x 直接拼接）退役**：代码保留在 `AssetTool/Core/RobotMerger.cpp` 但不调用，仅作参考实现（合并逻辑、Body_d 修正、Ry180、左手系转换的规则可被 fbxs2dxmesh 复用）。
2. **.bone 以 FBX 解析结果为准**：骨架树（name/parentIndex/TRS）从 Blender FBX 的 Armature 直接解析，不再从 HOD 拼装。FBX 骨骼名 `Body_d_bone` → 去 `_bone` 后缀、过滤 `_end` 末端节点（Blender 为显示骨骼方向加的辅助节点）。
3. **武器/挂点不建骨骼，走 socket 挂载模式**：原始 HOD 中武器（gun/sword）、挂点（Weapon_point、Output0x）、特效点可能有动画，但现代化路线下这些**不是骨骼**，而是独立 ECS 实体通过 `RelationshipComponent(kind=socket, socketName=...)` 挂到角色骨骼上（详见 `Docs/architecture/scene/RelationshipModel.md`）。骨骼只保留蒙皮所需（15 render 根）。
4. **ANI 直解也转 FBX，不直接产引擎资产**：`ani2anim`（ANI → 动画 FBX）作为动画进 Blender 工作流的桥；`.anim` 现代化（B2）从 Blender 动画 FBX 切分，不走 ANI 直解。

## 三、fbxs2dxmesh 转换器设计要点（P1）

| 事项 | 说明 |
|:-----|:-----|
| 子网格 | FBX 按材质槽拆 aiMesh，子网格数 = 材质槽数（合并材质后 38 → 5~6）；每子网格顶点刚性绑定 1 根骨骼（从顶点权重读，不靠文件名推断） |
| 骨骼索引 | 直接读 FBX 顶点权重（每 mesh 仅 1 根有权重）→ 写 `DxMeshSkinnedVertex.boneIndices` |
| 骨架 | Armature 树 → `.bone`；骨骼名去 `_bone` 后缀、过滤 `_end`；TRS 从 FBX 节点变换取 |
| 材质 | Blender Principled BSDF → 引擎 .mat（baseColor/metallic/roughness/ao） |
| 坐标系 | Blender FBX 默认 Y-up 右手系 → 引擎左手 Y-up（翻转 Z），规则复用 importrobot 既有实现 |
| 产出 | `.dxmesh`（skinned）+ `.bone` + `Materials/*.mat` + `Textures/*.dds` + `scene.json`，与 importrobot 输出同构 |

## 四、武器挂载动画数据来源（2026-08-01 确认：ANI 帧局部矩阵可直接复用）

> 背景：武器/盾（gun/sword/Shield）、挂点（Weapon_point、Output0x）、特效点在旧时代是骨架部件，随机体一起摆动作。现代化走 socket 挂载（不建骨骼），其自身层动画数据从 ANI 帧直接提取。

文档核实（`01_AssetFormatOverview.md` §二"动画"行、`02_RobotAndAnimation.md` §2.1）：

- **ANI 文件 = 文件名头 + 连续 HOD 块序列**（1.008 原版；PUK 2.008 为 HD2 块）
- **每个 HOD 块 = 一帧的部件局部矩阵快照**：部件名区 + A/B 层级 + **每部件局部 4×4 矩阵**（相对父部件，非世界矩阵）
- 帧矩阵语义 = **局部矩阵**（`RobotMerger.cpp` "层级累乘 local × parent"即基于此）

**推论——两层模型的自身层数据源现成**：

```
最终武器世界矩阵 = 机体父骨骼世界矩阵（机体动画层级累乘）× 武器局部矩阵（ANI 帧直提）

武器局部矩阵 = ANI 帧里 gun/sword/Shield 部件的局部 4×4 矩阵
             → 直接作为挂载实体自身层动画（SocketAnimComponent）的 .anim 剪辑数据
             → 零转换、零丢失，"旧时代一起摆动作"的表现完整迁移
```

**待办影响**：武器挂载动画数据提取（从 ANI 帧局部矩阵 → 武器自身 .anim 剪辑）作为独立任务列入待办（见下表 U8），与机体动画 B2（.anim 现代化）并行——两者数据来源相同（ANI 帧），只是切分维度不同（机体骨骼通道 vs 武器部件通道）。

## 五、引擎侧下一步（待办）

| # | 任务 | 优先级 | 说明 |
|:-:|:-----|:-------|:-----|
| 1 | **fbxs2dxmesh 转换器** | ✅ 已完成 | 最终 FBX → .dxmesh/.bone/.mat/scene.json；kd-03 实测：8 子网格/3045 顶点/15 骨骼 |
| 2 | **SkeletonManager 加载 .bone** | ✅ 已完成 | `LoadFromJSON` 已实现（读 bones 数组：name/parentIndex/position/rotation/scale），`SkeletonLoadTask` 异步路径就绪 |
| 3 | **SkinnedComponent 接入** | P1 | 场景加载创建 SkinnedComponent（骨架引用 + 网格引用），`SceneConstructor` 框架已有；⚠️ 生成器暂不改（分工定案）→ 先走动画视口预览，NPC 场景化留 P2 |
| 4 | **蒙皮渲染链路** | P1 | GPU 骨骼缓冲（StructuredBuffer<float4x4>）+ 蒙皮着色器绑定（`DxMeshSkinnedVertex` 的 R8G8B8A8_UINT boneIndices + boneWeights）；`SkinnedRenderer`/`SkinnedRenderItemBuilder` 已存在，缺口：骨骼缓冲上传（AnimationAdvancer）+ 子网格展开 |
| 5 | **动画资产 B2（.anim）** | P1 | 格式规范见 `Docs/architecture/animation/AnimationAsset.md`（新）；AssetTool `anim2clip` 转换器（FBX AnimStack → .anim）+ `AnimationManager`/`AnimLoader` |
| 6 | **动画视口** | P1 | 见 `Docs/architecture/animation/AnimationViewport.md`（新）；角色/动画专用查看面板（播放/调帧/骨骼调试），角色不进主 viewport |
| 7 | **武器挂载动画数据提取（U8）** | P2 | 从 ANI 帧局部矩阵提取 gun/sword/Shield 部件通道 → 武器自身 .anim 剪辑（SocketAnimComponent 数据源） |
| 8 | **IK 入引擎** | P2 | IKSolver 已离线验证（B 方案），引擎侧接 FABRIK 需先清蒙皮管线缺口（即 3-4） |
| 9 | **socket 挂载消费** | P2 | 角色骨骼 → 外部实体 socket 挂载的运行时解算（配合蒙皮骨骼世界矩阵） |
| 10 | **.character 复合资产 + NPC 场景化** | P2 | `.character` 打包 mesh/bone/materials/clips；场景 `character` 组件（此时才改 SceneConstructor） |

## 六、推进步骤（2026-08-01）

按依赖关系排序的落地顺序：

```
阶段 1（资产侧，✅ 完成）：
  ① fbxs2dxmesh 转换器          ← Blender 最终 FBX → .dxmesh/.bone/.mat/scene.json
  ② 合并材质后重新导出 kd-03.fbx  ← 用户 Blender 操作，38 材质 → 8 唯一材质，子网格表干净
阶段 2（动画资产，P1，进行中）：
  ③ anim2clip 转换器            ← 动画 FBX AnimStack → .anim（AnimationAsset.md §四）
  ④ AnimationManager/AnimLoader ← .anim 加载/注册/ClipHandle
  ⑤ ComputeFinalTransforms 改造  ← clipName → ClipHandle（AnimationAsset.md §5.3）
阶段 3（蒙皮渲染链路，P1）：
  ⑥ 骨骼缓冲上传（AnimationAdvancer）← 每帧采样 → GPU 骨骼缓冲
  ⑦ SkinnedRenderItemBuilder 子网格展开
  ⑧ 动画视口（AnimationViewportPanel）← 查看角色/动画，不进主 viewport
阶段 4（角色场景化/挂载，P2）：
  ⑨ .character 复合资产 + NPC 场景化  ← 此时才改 SceneConstructor
  ⑩ 武器挂载动画提取（U8）        ← ANI 帧局部矩阵 → 武器 .anim（与 ③ 同源）
  ⑪ socket 挂载消费（三层模型）   ← 依赖 ⑥⑩，见 RelationshipModel.md §四
  ⑫ IK 入引擎                   ← 依赖 ⑥
```

**建议顺序**：③ → ④ → ⑤ → ⑥ → ⑦ → ⑧（先打通 .anim 资产 + 动画视口静态预览，再补骨骼缓冲与子网格展开），之后进入 ⑨-⑫（角色场景化/挂载）。

## 七、相关文件

- 转换器（退役参考）：`AssetTool/Core/RobotMerger.cpp`（Merge/importrobot）、`RobotMergerFBX.cpp`（ani2anim/二进制 FBX，保留）
- 引擎骨架：`Engine/.../SkeletonManager.h/.cpp`、`SkeletonData.h`
- 顶点格式：`DxMeshSkinnedVertex`（64B：position/tangentU/normal/texC/boneWeights/boneIndices）
- 挂载模式：`Docs/architecture/scene/RelationshipModel.md`（socket 两层模型 §四 + ANI 帧局部矩阵 §4.5.1）
- ANI 格式：`Docs/targets/UKW_PowerUpKit/02_RobotAndAnimation.md`（§2.1 HOD 块结构、§6.2 骨骼变换公式）
