# UKW 引擎侧资产管线推进方向

> 日期：2026-08-01
> 状态：📋 方向定案（转向引擎侧）
> 关联：`06_SubMeshPipeline.md`（SubMesh 合并管线）、`AssetTool/Core/RobotMerger.cpp`（importrobot 实现）、`AssetTool/Core/RobotMergerFBX.cpp`（ani2anim 实现）
> 范围：引擎侧蒙皮/骨架管线（A3 skinned 顶点已就绪、C SkeletonManager、D SkinnedComponent），DCC 侧已定案（骨骼视觉断开接受，仅二进制 FBX 导出有效）

---

## 一、DCC 侧定案回顾（2026-08-01）

- **骨骼视觉断开是 UKW 数据固有**：部件变换矩阵语义（非关节链几何），多分支父骨骼无法全连；任何 head/tail 调整会破坏蒙皮/动画配对 → **接受现状**
- **唯一有效改动**：FBX 导出改**二进制格式**（Blender 5.2 不支持 ASCII），导入勾选 Force Connect Children 单链可连接
- 用户已在 Blender 中**合并子网格后重新导出**：`C:\Users\32199\Desktop\kd-03.fbx`（1 Armature / 20 骨骼 / **15 mesh** / 38 材质，原 38 子网格按材质拆分 → Blender 合并为每骨骼 1 个 mesh）

## 二、两条 dxmesh 路径对比

### 路径 A：.x 直接拼接（importrobot，推荐，已实现）

```
Robo/KD-03/Robo.hod + Body.x/Head.x/arm1.x... 
  → AssetTool importrobot（RobotMerger::Merge）
  → KD-03.dxmesh（DxMeshSkinnedVertex 蒙皮格式，局部坐标 + 刚性绑定）
  → KD-03.bone（引擎正式骨架，左手系 Y-up，Body_d 修正 + 层级累乘 + Ry180）
  → KD-03.hod.json（Z-up 预览用）+ Materials/*.mat + Textures/*.dds + scene.json
```

**状态**：✅ **A3 已就绪**——`RobotMerger.cpp` §7 已用 `DxMeshSkinnedVertex`（boneWeights={1,0,0,0} + boneIndices），skinned=true 写入，不解烘焙（06 文档的 ❌ 标记已过时）。CLI 命令 `importrobot <Robo.hod> <output_dir> [--no-lrswap]`。

### 路径 B：合并后 FBX → dxmesh（新转换器，暂缓）

```
kd-03.fbx（Blender 合并后，15 mesh）
  → 需新增 fbxs2dxmesh 转换器（assimp 读 FBX）
  → .dxmesh + .bone + .mat
```

**优点**：mesh 已合并（每骨骼 1 个），子网格表更干净；可承载 Blender 端修模/优化
**代价**：需开发 FBX 导入转换（当前 AssetTool 有 assimp 但无 FBX→dxmesh 路径）；材质经 Blender 往返（38 材质但 15 mesh，材质→mesh 映射需按名字对齐）；骨骼 20 根 vs render 15 根（含 root/辅助节点需过滤）

## 三、决策建议

**近期用路径 A**（importrobot 已完整实现，输出 .dxmesh/.bone/.mat/scene.json 全套），引擎侧聚焦消费这些资产：
- 网格：`KD-03.dxmesh`（DxMeshSkinnedVertex 刚性绑定）
- 骨架：`KD-03.bone`（左手系，name 含 .x 后缀与 ANI 通道对应）
- 材质：`Materials/KD-03_*.mat`（PBR 参数，含 baseColor/metallic/roughness/ao）

**FBX → 资产转换（路径 B）目前不需要**，留作后续（如需要 Blender 修模成果回流时）再开发。

## 四、引擎侧下一步（待办）

| # | 任务 | 优先级 | 说明 |
|:-:|:-----|:-------|:-----|
| 1 | **SkeletonManager 加载 .bone** | P1 | 引擎侧从 `.bone` JSON 加载骨架树（现有 `SkeletonManager.h/.cpp` 为骨架实现，需补 `LoadFromJSON` 读 bones 数组：name/parentIndex/position/rotation/scale） |
| 2 | **SkinnedComponent 接入** | P1 | 场景加载时创建 SkinnedComponent（骨架引用 + 网格引用），`SceneConstructor` 框架已有 |
| 3 | **蒙皮渲染链路** | P1 | GPU 骨骼缓冲（StructuredBuffer<float4x4>）+ 蒙皮着色器绑定（`DxMeshSkinnedVertex` 的 R8G8B8A8_UINT boneIndices + boneWeights） |
| 4 | **动画资产 B2（.anim 现代化）** | P2 | ANI 解析器已完（B1），下一步全量转切分剪辑 + AnimLoader（`CharacterAsset.md` §九） |
| 5 | **IK 入引擎** | P2 | IKSolver 已离线验证（B 方案），引擎侧接 FABRIK 需先清蒙皮管线三段缺口（即上表 1-3） |

## 五、相关文件

- 转换器：`AssetTool/Core/RobotMerger.cpp`（Merge/importrobot）、`RobotMergerFBX.cpp`（ani2anim/二进制 FBX）
- 引擎骨架：`Engine/.../SkeletonManager.h/.cpp`、`SkeletonData.h`
- 顶点格式：`DxMeshSkinnedVertex`（64B：position/tangentU/normal/texC/boneWeights/boneIndices）
