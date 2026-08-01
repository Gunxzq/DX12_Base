# UKW 资产管线：DCC 定案 + 引擎侧方向快照 (2026-08-01)

> UKW PowerUp Kit 资产管线：DCC 侧（FBX 导出/Blender 验证）定案 + 引擎侧（dxmesh/骨架）推进方向
> 关联：`Docs/targets/UKW_PowerUpKit/07_EngineAssetPipeline.md`（引擎侧方向）、`06_SubMeshPipeline.md`（合并管线）、`02_RobotAndAnimation.md`（ANI/HOD 分析）

---

## 一、DCC 侧定案（2026-08-01）

### 1.1 问题背景

FBX 导入 Blender 后骨骼视觉断开（互不连接的短棒），Pose 模式调整受限。根因：HOD 矩阵描述**部件变换**（非关节链几何），FBX 骨骼节点只有矩阵，Blender 靠矩阵推导 head/tail，部件间有物理间隙 → 视觉断开。**与数据正确性无关，蒙皮/动画本身正常。**

### 1.2 尝试过的方案与结论

| 方案 | 结果 |
|:-----|:-----|
| 导出 joints.txt + Blender 脚本重建 head/tail（8c 块） | ❌ 多分支父骨骼（Body 有 5 子）无法全连；强制连接会移动 head 破坏蒙皮 |
| Blender 原生 Force Connect Children（方案 A） | ⚠️ 单链（臂/腿 3 节）可连（8/15 use_connect），多分支处仍断开 |
| 手动重建 rig（07 旧文档） | ❌ 经实测与现状无差别，废弃 |

**定案**：骨骼视觉断开是 UKW 数据固有（部件变换矩阵语义），**接受现状**；唯一有效改动是 FBX 导出改**二进制格式**（Blender 5.2 不支持 ASCII FBX）。

### 1.3 代码改动（已编译验证）

| 文件 | 改动 |
|:-----|:-----|
| `RobotMergerFBX.cpp` | 导出 `"fbxa"→"fbx"`（二进制）；删除 8a ASCII 文本替换块（二进制下会破坏文件）；删除 8c 块（joints.txt/armature_fix.py，免脚本） |
| `RobotMergerUtil.cpp` | `CreatePartMaterial` 把 emissive 合入 diffuse（`min(1.0, face+emissive)`），自发光件（黄绿）导入 Blender 不再全黑 |
| `07_DCC_RigRebuild.md` / `08_DCC_JointExport_Plan.md` | 已删除（文档清理） |

### 1.4 用户后续操作

用户已在 Blender 中**合并子网格**（38 子网格按材质拆分 → 每骨骼 1 mesh，共 **15 mesh**）后重新导出：`C:\Users\32199\Desktop\kd-03.fbx`（1 Armature / 20 骨骼 / 38 材质）。

## 二、引擎侧方向（下一步）

### 2.1 两条 dxmesh 路径

**路径 A：.x 直接拼接（importrobot，推荐，已实现）**

```
Robo/KD-03/Robo.hod + Body.x/Head.x/arm1.x...
  → AssetTool importrobot（RobotMerger::Merge）
  → KD-03.dxmesh（DxMeshSkinnedVertex 蒙皮格式：局部坐标 + 刚性绑定 boneWeights={1,0,0,0}）
  → KD-03.bone（引擎正式骨架，左手系 Y-up，Body_d 修正 + 层级累乘 + Ry180）
  → KD-03.hod.json + Materials/*.mat + Textures/*.dds + scene.json
```

**状态**：✅ **A3 已就绪**——`RobotMerger.cpp` §7 已用 `DxMeshSkinnedVertex` + skinned=true 写入（06 文档的 ❌ 标记已过时）。CLI：`importrobot <Robo.hod> <output_dir> [--no-lrswap]`。

**路径 B：合并后 FBX → dxmesh（暂缓）**

Blender 合并后的 kd-03.fbx（15 mesh）→ 需新增 fbxs2dxmesh 转换器（assimp 读 FBX）。优点：mesh 已合并、子网格表干净；代价：需开发转换 + 材质映射 + 骨骼过滤（20 vs 15 根）。**目前不需要，留作后续 Blender 修模成果回流时再开发。**

### 2.2 引擎侧待办（详见 07_EngineAssetPipeline.md §四）

1. **SkeletonManager 加载 .bone**（P1）：现有 SkeletonManager.h/.cpp 为骨架实现，补 LoadFromJSON（name/parentIndex/position/rotation/scale）
2. **SkinnedComponent 接入**（P1）：场景加载创建 SkinnedComponent，SceneConstructor 框架已有
3. **蒙皮渲染链路**（P1）：GPU 骨骼缓冲（StructuredBuffer<float4x4>）+ 蒙皮着色器（R8G8B8A8_UINT boneIndices）
4. **动画资产 B2（.anim 现代化）**（P2）：ANI 解析已完（B1），下一步全量转切分剪辑 + AnimLoader
5. **IK 入引擎**（P2）：IKSolver 已离线验证（B 方案），需先清蒙皮管线缺口（即 1-3）

## 三、关键文件

```
AssetTool/
  ├─ Core/RobotMerger.cpp        ← importrobot（Merge → .dxmesh/.bone/.mat/scene.json）
  ├─ Core/RobotMergerFBX.cpp     ← ani2anim（ANI → 二进制 FBX + 帧矩阵 dump）
  ├─ Core/RobotMergerUtil.cpp    ← CreatePartMaterial（emissive 合入 diffuse）
  ├─ Core/ANIParser.cpp          ← ANI 解析（1.008 + PUK 双格式）
  ├─ Core/IKSolver.cpp           ← FABRIK 离线验证（B 方案）
  └─ CLI/main.cpp                ← importrobot/ani2anim/ani2ik 命令入口
Docs/targets/UKW_PowerUpKit/
  ├─ 06_SubMeshPipeline.md       ← SubMesh 合并管线（A3 已就绪，待更新状态）
  └─ 07_EngineAssetPipeline.md   ← 引擎侧方向（新）
```
