# AssetTool 导出管线快照 (2026-07-31)

> 当前状态：GUI 已同步 Core 管线，全部导出功能就绪

---

## 已完成

### 1. GUI 同步 Core（RobotMerger）
- 删除 GUI (`AssetToolGUI.cpp`) 中 ~680 行内联重复代码（`PartData`、`Mat4x4`、骨骼矩阵、FBX/.x/dxmesh 导出）
- 改用 `RobotMerger::Merge()` — 与 CLI 侧完全一致的公共 API
- 异常安全：空路径守卫 + try-catch 兜底，线程不崩

### 2. .x 嵌套层级导出 ✅
- 输出：`Robo.x`（29 骨骼嵌套层级 + 网格）
- 局部矩阵公式：`boneWorld[子] × inverse(boneWorld[父])`（行主序 post-multiply）
- boneWorld 公式：HOD 原始矩阵 → Body_d 偏移 → 层级累乘 → Ry180° → Y-up 世界空间
- LR 交换：交换 name + mesh（不交换 boneIndex/矩阵）
- 验证：Hand_r 位置 `(0, 0, -0.295775)` 与社区版逐位匹配

### 3. FBX 导出 ✅
- **骨骼过滤**：只导出有网格的骨骼及其祖先，排除武器/喷射口/root 等空骨架节点，Blender 中不再出现怪异线条
- **蒙皮绑定**：顶点保持局部坐标，刚性蒙皮 `aiBone`（weight=1.0）+ offsetMatrix，骨骼移动时 mesh 正确跟随
- **层级**：网格直接挂在对应骨骼节点下，不再堆叠在 meshRoot
- **子网格**：FBX 通过 `aiMesh` 自身即可表达子网格（每个部件一个 aiMesh），无需额外子网格表

### 4. dxmesh 蒙皮格式 ⚠️
- 顶点格式：`DxMeshSkinnedVertex`（64B，刚性绑定）
- 保持局部坐标，不解烘焙
- 每顶点设 `boneWeights = {1,0,0,0}` + `boneIndices = {partBoneIndex, 0,0,0}`
- 写入 `skinned = true`
- **左手系**：`leftHanded` 默认 `true`，顶点 Z + 骨骼矩阵 Z 列同步取反

### 5. 平铺版弃用
- 仅保留嵌套 `.x` 导出（`Robo.x`）

---

## 关键文件

| 文件 | 作用 |
|:-----|:------|
| `AssetTool/Core/RobotMerger.h` | Merge API、`RobotMergeOptions`（含 `leftHanded`）、`ProgressCallback` |
| `AssetTool/Core/RobotMerger.cpp` | 合并管线主逻辑（.x/FBX/dxmesh），FBX 骨骼过滤 + 蒙皮，左手系转换 |
| `AssetTool/CLI/main.cpp` | CLI 入口（`CommandImportRobot`） |
| `AssetTool/GUI/AssetToolGUI.cpp` | GUI 入口（已同步 Core，无内联重复代码） |

## 验证数据

- 社区版 Hand_r（嵌套局部矩阵）：`Position (0, 0, -0.295775)`
- 导出版 Hand_r（嵌套局部矩阵）：`Position (0, 0, -0.295775)` ✅ 匹配
- FBX 骨骼蒙皮：骨骼旋转时 mesh 正确跟随 ✅

---

## 已知问题 / 待办

### 1. dxmesh 仍挤在一起 ❌

> **确认根因**：AssetTool 导出的 dxmesh 骨骼数据完整，问题在引擎运行时未接通蒙皮管线（非数据格式问题）。

确认经过：RobotMerger 导出的 `.dxmesh` 使用 `DxMeshSkinnedVertex`（64 字节），含 `boneWeights={1,0,0,0}`、`boneIndices={partBoneIndex,0,0,0}`，并设置 `DxMeshFlag_Skinned`。顶点格式和 SubMesh 表均已正确写入。

堆叠的原因是引擎侧三段缺口（详见 `Docs/architecture/SkeletalMeshAssetPipeline.md` §二）：
1. **SkeletonManager 无 hod.json 加载** — 骨架数据读不进内存
2. **GPU 骨骼缓冲未接通** — 骨骼矩阵送不到 GPU
3. **没有蒙皮着色器绑定** — SkinnedRenderer 的 BoneBuffer SRV 槽位空置

**修复后验证**：引擎侧补齐上述三段后，当前导出的 dxmesh 无需任何格式改动即可正确渲染。

### 2. 复合资产定义（含动画）
- 当前 AssetTool 输出的是原子资产（dxmesh + hod.json + 材质），未包含动画数据
- 需要设计复合资产格式，整合 HOD 骨骼动画 + mesh + 材质为完整的 Scene/Model 资产

### 3. dxmesh 左手系验证
- 顶点和矩阵的转换逻辑已实现，但缺乏引擎端验证
- 需等到引擎蒙皮管线就绪后，配合测试确认结果

## 设计决策

- **武器/喷射口排除**：这些部件在 HOD 中保留完整骨骼数据，但 FBX/.x/dxmesh 导出时跳过。引擎通过挂载系统加载独立武器模型
- **root 排除**：root 是恒等矩阵的空骨架节点，FBX 中移除，子骨骼直接挂载到场景根节点
- **坐标系统**：导出默认左手系（`leftHanded=true`），匹配引擎 DirectX 风格左手 Y-up
