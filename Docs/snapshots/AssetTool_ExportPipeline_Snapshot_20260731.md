# AssetTool 导出管线快照 (2026-07-31)

> 当前状态：.bone 骨架资产导出完成并验证通过（Y-up，与 nested.x 逐位一致）；**ANI 解析完成**（1.008 + PUK 2.008 双格式，标记法 + 母版驱动，25 机体全量拆解成功）；**动画适配已统一到 1.008**（ani2frames 与 GUI nested.x 逐字节一致、ani2anim FBX 动画通道修复 + 骨骼树修复 + 时间轴按 Tail 10fps，见 §10/§11）；`.anim` 现代化资产为下一任务

---

## 已完成

### 1. GUI 同步 Core（RobotMerger）
- 删除 GUI (`AssetToolGUI.cpp`) 中 ~680 行内联重复代码（`PartData`、`Mat4x4`、骨骼矩阵、FBX/.x/dxmesh 导出）
- 改用 `RobotMerger::Merge()` — 与 CLI 侧完全一致的公共 API
- 异常安全：空路径守卫 + try-catch 兜底，线程不崩

### 2. .x 嵌套层级导出 ✅
- 输出：`Robo_nested.x`（29 骨骼嵌套层级 + 网格）
- 局部矩阵公式：`boneWorld[子] × inverse(boneWorld[父])`（行主序 post-multiply）
- boneWorld 公式：HOD 原始矩阵 → Body_d 偏移 → 层级累乘 → Ry180° → Y-up 世界空间
- LR 交换：交换 name + mesh（不交换 boneIndex/矩阵）
- 验证：Hand_r 位置 `(0, 0, -0.295775)` 与社区版逐位匹配

> **坐标系说明**：nested.x 存储为**左手系 Y-up**（引擎空间）；`(0, 0, -0.295775)` 为社区版工具 **Z-up 显示视角**下的同一物理位置，对应 nested.x 第 4 行（行主序平移）`0,-0.295775,~0`，即 Y-up 存储 `(0, -0.295775, ~0)`。社区版 Z-up 仅用于建模工具对比，**引擎直接消费的资产（.bone/.dxmesh）必须为 Y-up**。

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
- 仅保留嵌套 `.x` 导出（`Robo_nested.x`）

### 6. .bone 骨架资产导出 ✅
- **格式**：`{ version, bones:[{name, parentIndex, position, rotation, scale}] }`（JSON，`CharacterAsset.md` §四）
- **变换链路**：HOD 原始矩阵 → Body_d 修正 → 层级累乘 → Ry180° → Z 列取反（左手系）→ 局部矩阵 `world[child] × inverse(world[parent])` → TRS 分解
- **坐标系**：Y-up 引擎空间（与 dxmesh 顶点一致）；name 保留 HOD 原部件名（含 `.x` 扩展名），与 ANI 动画通道名对应
- **无条件输出**（不依赖 GUI 验证格式复选框；复选框仅控制 .x/FBX）
- **验证**：Body_d / Head / arm2_r / Hand_r 的 position 与 `nested.x` 平移行逐位一致；Hand_r `(0, -0.295775, ~0)` ✅

### 7. ANI 解析（1.008 + PUK 2.008 双格式）✅
- **新解析器**：`AssetTool/Core/ANIParser.h/.cpp` + CLI `ani2output` 命令 + GUI "ANI 拆解 → HOD + Tail.txt" 按钮
- **标记法 + 母版驱动，无固定步长**：
  - 帧边界 = `.hod` 文件名（1.008/PUK 标准）或 `HD2` 魔术（无 `.hod` 标记变体，如 KD-04_4）
  - 帧数据结束 = 下一帧名字起点（= 下一帧 `.hod` 位置 − 名字长度）
  - 组尾判定 = Tail 明文特征（`IF(@` + 16B 头）；空 Tail 组用"部件数 × 每部件大小"兜底
  - 母版（HD2 类型=0）含完整骨架（部件名 + A/B + 数据），可替代外部 `Robo.hod`
- **格式兼容**：
  - 1.008 原版：`HOD` 块，帧数据 = 7+N×328（KD-03：65 组 347 帧）
  - PUK 2.008：`AN2`+`HD2` 块，帧数据 = N×179-8+19（KD-03：66 组 357 帧；KD-04_4 变体：51 部件 9140B，74 组 289 帧）
- **Tail 输出**：16B 头 + 明文 SPT（Shift-JIS），段落格式（`---NNN---` + 时间/速度 + Begin/End）与社区版一致
- **验证**：25 机体全量拆解成功（成功 25 / 失败 0）；KD-03 输出与社区拆解产物逐字节一致（HOD 帧 md5 + Tail.dat）
- **调试日志**：输出目录 `ani_debug.log`（输入/诊断/成功失败汇总）

### 8. ANI 母版管线（合并部件新入口）✅
- **`ANIParser::GetMaster()`**：解析母版块（HD2 类型=0 / HOD 首块）→ 部件名 + A/B 层级（`ANIMaster` 结构）
- **`RobotMerger::MergeFromANI(aniPath, outputDir)`**：母版驱动合并新入口——解析 ANI 母版骨架 → 定位同目录 Robo.hod（绑定矩阵）→ 校验母版/HOD 部件数一致性 → 复用 HOD 合并管线输出 x/fbx/bone
- **步骤划分**（导入机体合并部件功能调整）：
  1. 解析 ANI 母版拿骨骼（部件名 + A/B）✅
  2. 合并部件输出母版对应 x/fbx/bone（bone 同样输出）✅
  3. 全部动画数据 + Tail 按动画组分隔输出到单一 TXT（不分文件夹）🔜 下一任务（B2.5）
  4. 最终目标：含动画信息的 FBX + 状态机 Tail 文本
- **已知限制**：PUK 母版数据区（135B/部件）是 TRS 编码（非 4×4 矩阵），绑定矩阵暂用同目录 Robo.hod；TRS 格式待逆向后可彻底脱离 HOD

### 9. 动画帧 → FBX 转换链路设计（B2.5）🔜
- **动画数据源已实测确认**：**1.008 拆解帧 = 标准 HOD**（9847B，HODParser 直接可解析，无需逆向）；**PUK 帧块** = HD2 紧凑格式（19B 头 + 每部件 179B = 171B TRS + 8B A/B），ANIParser 已按此结构拆解（A/B @ +171 提取成功）；walk 组 8 帧 Body_d 的 `f[1]`（Y 位移 0.0436→0.0698→-0.0436...）、`f[8]`（身体高度 1.4022→1.3241→1.3999...）随帧变化 = 动画轨迹 ✅
- **转换链路（HODParser 直解，无需逆向 TRS）**：HODParser 解析帧数据（1.008 直喂 / PUK 由 ANIParser 拆出 TRS）→ 每帧部件局部矩阵 → 层级累乘 boneWorld（§6.2 公式）→ 世界转局部（world×inverse(parent)）→ TRS 分解 → 每骨骼 aiNodeAnim（position/rotation/scale 关键帧）→ 每动画组一个 aiAnimation → Assimp 导出含动画 FBX
- **待确认点**：时间轴来源（Tail time/speed 或固定帧率）、动画关键帧为绝对 TRS 还是相对绑定姿势增量、多动画组单 FBX vs 分文件
- 详见 `02_RobotAndAnimation.md` §2.5

> **现状（2026-07-31 23:11 实测，1.008 适配后）**：
> - **`ani2anim`**：输出 `Robo_anim.fbx`（**65 clips / 15 bones / 15 meshes**，ASCII FBX），网格已合并、武器/发射口/root 已过滤（29→15 骨骼），组名唯一化
> - **`ani2frames`**（调试/观察用）：每帧组装嵌套 x → 1.008 全量 347 帧导出（65 组）
> - **动画通道修复**：Body_d 动画关键帧 Y = `0.1022→0.024→0.031→0.0999→0.010→0.023→0.1022`（walk 组），与绑定姿势 0.062389 同数量级且随帧起伏——旧问题（关键帧 Y=1.27~1.40 与绑定姿势不一致）已消除 ✅
> - **已解决**：组名重复（Blender 不再合并 AnimStack）、动画坐标系不一致（见 §10）、骨骼树缺失（见 §11）
> - **⚠️ web 兼容性**：**大部分 web FBX 查看器无法识别**该动画 FBX（含骨骼树/动画通道时尤为明显）；**Blender 5.x 可正常识别播放**——结论：web 端不作为本资产验证目标，以 Blender 为准（见 §11）

> **BugFix（2026-07-31）：TRS 分解转置错误**
> 根因：`XMMatrixDecompose` 按 DirectXMath **行主序**约定从第 4 行（r[3]）提取平移，但 `HODParser::DecomposeTRS` 与 `RobotMerger` 9b 块此前先执行 `XMMatrixTranspose`，把平移移到第 4 列，导致 position 恒为 0（hod.json 的 position 全 0 也是同一历史 bug，预览格式未暴露）。
> 修复：两处均移除 `XMMatrixTranspose`，直接分解行主序矩阵。修复后 .bone position 与 nested.x 逐位一致。
> 佐证：FBX/nested.x 直接写 4×4 矩阵（不经 TRS 分解），一直正确——问题仅在 .bone 分支。

### 10. ANI 动画适配统一到 1.008（动画通道修复）✅

> **背景**：PUK 2.008 帧块（HD2 TRS 编码）与 1.008 帧块（标准 HOD 4×4）虽存储相同姿势，但**存储矩阵语义不一致**——HOD 帧矩阵为左手系（含 Z 镜像，det=-1），HD2 TRS 反解为右手系（det=+1），直接混用导致 ani2frames/ani2anim 输出旋转符号相反（arm1 符号相反、Body_d m[2]/m[10] 翻转）。社区对照基准（GUI nested.x）全部基于 1.008 原版 ANI，故**适配统一到 1.008，2.008 不做特殊处理**。

- **代码修改**（`RobotMerger.cpp` `ParseFrameToLocalMatrices`）：
  - isHod 分支（1.008）：HODParser 直解帧矩阵，补 **Body_d -1.30 偏移修正**（与静态 Merge 组装一致，帧数据 Y 同样含 +1.30 偏移）
  - isHd2 分支（2.008）：仅补 Body_d -1.30 修正，**不做 Z 镜像/转置适配**（社区不使用，无验证基准）
- **数据替换**：将 `C:\Users\32199\Desktop\tool\1.008原版ani` 的 `Script.ani` 替换到 `D:\APP\Ultimate Knight WindomXP PowerUp Kit\Robo` 与 `C:\Users\32199\Desktop\tool\data\Robo` 两个目录（17 机体 × 2 目录，md5 全部一致；KD-04_4/KD-05_4/KD-05_5/KD-08_5~7_2/senkan01 等 2.008 专属机体保留原数据）
- **验证（KD-03 walk01 第 1 帧）**：
  - ani2frames 输出与 GUI nested.x **逐字节一致**（diff 0 差异，含全部骨骼矩阵 + 网格/法线/UV）✅
  - ani2anim 输出 `Robo_anim.fbx`：**65 clips / 15 bones / 15 meshes**；Body_d 动画关键帧 Y = `0.1022→0.024→0.031→0.0999→0.010→0.023→0.1022`（walk 组，随帧起伏），与绑定姿势 0.062389 同数量级 → **动画通道坐标系修复** ✅
- **关键经验**：HD2 帧块 TRS 与 HOD 帧矩阵并非同一存储语义（差 Z 镜像/转置），跨格式混用前必须先确认"存储矩阵是否一致"，且对比基准必须与目标数据源同版本

### 11. FBX 骨骼树修复 + 动画时间轴（Blender 可播放）✅

> **背景**：Blender 导入动画 FBX 后大纲无骨架、每个部件独立动画、无动画列表、无法播放。根因是导出结构缺失骨骼树——两处 FBX 导出均用 `boneNodes[bi]->mNumMeshes = 1` 把网格**直接挂到骨骼节点**，Assimp 把所有节点导出为 `Mesh` 类型，FBX 中**没有任何骨骼节点**，Blender 无法构建 Armature。另：动画时间轴此前硬编码 30fps，而 docs 实测 Tail 头速度值 = 帧间隔秒数（0.1f → **10 FPS**）。

- **代码修改**（`RobotMerger.cpp`）：
  - **骨骼树**（Merge 6b + ExportAnimationsFBX 第 6 节）：网格改为挂到骨骼节点的**子节点**（`aiNode *meshNode`），骨骼节点保持无 mesh → FBX 导出为 `LimbNode`/`Null` 类型 → Blender 识别 Armature
  - **时间轴**（ExportAnimationsFBX）：`ticksPerSec = 1 / g.tail.speed`（Tail 头速度值 = 帧间隔秒数，docs 0.1f → 10 FPS），带范围校验 `(0.001, 1.0)`——垃圾值（空 Tail 组/定位偏差读出 0 或 1e33）回退默认 30fps
  - **帧矩阵 dump**（ExportAnimationsFBX 8b）：导出 FBX 时同目录附带 `{stem}_anim_frames.txt`（逐组逐帧逐骨骼局部矩阵 16 元素，供与社区拆解对照）
- **验证（KD-03，Blender 实测）**：
  - FBX 结构：**15 `LimbNode` 骨骼 + 15 `Mesh` 子节点**，骨骼树 `Body_d→Body→Head/arm/leg` 层级完整，蒙皮 Skin→Cluster→骨骼 连接正确 ✅
  - Blender 大纲出现 **Armature**（骨骼树层级），选中骨架顶层节点可切换 65 个动画 ✅
  - 动画列表 = 65 个 Action（robo_stand_1、robo_walk01_2…），关键帧数与组帧数逐组一致（3~39 帧）✅
  - 动画可播放（10fps 源数据特性：walk 9 帧 ≈ 0.9s，属正常）✅
- **⚠️ web 兼容性结论**：**大部分 web FBX 查看器无法识别**该动画 FBX（含骨骼树 + 动画通道时识别失败）；**Blender 5.x 可正常识别播放**。此前"二进制→ASCII 保证 web 兼容"的假设不成立——ASCII 仅解决部分查看器的解析，动画/骨骼结构仍是多数 web 工具短板。**验证目标以 Blender 为准，web 端不作为本资产验证目标**。
- **Blender 操作要点**：
  - 动画短（3~39 帧 @10fps）是源数据特性，可勾选 NLA 轨道 Cyclic 循环播放
  - NLA 编辑器只显示选中对象的轨道：选中 Armature 后再看，或勾选 Show → Summary
  - 隐藏骨骼：大纲 Armature 行点眼睛图标，或 Viewport Display 取消 In Front（mesh 蒙皮不受影响）
- **遗留**：`Robo_anim_frames.txt` 中部分组（group 24-46 等 S 变体）Tail 头解析出垃圾值（tailTime=2.8e9 等）→ 时间轴回退 30fps；根因疑为 ANIParser `ParseTail` 定位偏差，后续可单独排查

---

## 关键文件

| 文件 | 作用 |
|:-----|:------|
| `AssetTool/Core/RobotMerger.h` | Merge API、`RobotMergeOptions`（含 `leftHanded`）、`ProgressCallback` |
| `AssetTool/Core/RobotMerger.cpp` | 合并管线主逻辑（.x/FBX/dxmesh/.bone），FBX 骨骼过滤 + 蒙皮，左手系转换 |
| `AssetTool/Core/HODParser.cpp` | HOD 解析 + TRS 分解（DecomposeTRS，转置 bug 已修复） |
| `AssetTool/CLI/main.cpp` | CLI 入口（`CommandImportRobot`） |
| `AssetTool/GUI/AssetToolGUI.cpp` | GUI 入口（已同步 Core，无内联重复代码；LR 复选框已删，.x/FBX 合并为一个验证导出开关） |

## 验证数据

- 社区版 Hand_r（嵌套局部矩阵）：`Position (0, 0, -0.295775)`（社区版工具 **Z-up 显示视角**）
- 导出版 Hand_r（嵌套局部矩阵）：nested.x 平移行 `0,-0.295775,~0` → **Y-up 存储 `(0, -0.295775, ~0)`**，与社区版 Z-up 视角同一物理位置 ✅
- **.bone Hand_r**：`position (0, -0.295775, ~0)`，与 nested.x 逐位一致 ✅
- **.bone 各骨骼**：Body_d `(0, 0.062389, 0)` / Head `(0, 0.278625, -0.010406)` / arm2_r `(-0.094451, -0.184366, ~0)`，均与 nested.x 平移行一致 ✅
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

### 2. ANI → .anim 现代化资产（下一会话第一任务）🔜
- 目标：ANI 解析已完成（按组提取"帧数据 + Tail 状态机"），**下一步**是把帧矩阵 + 母版骨架转换为现代化 .anim 资产（`.anim` 格式 + AnimLoader）
- 前提：ANI 样本文件已可全量拆解（**统一使用 1.008 原版 ANI**，已替换到 D:\APP 与 Desktop data 两目录；PUK 2.008 不再作为适配目标）；社区工具有现成对照基准（`02_RobotAndAnimation.md` §2.3）
- HOD 结果（.bone）已可作**初始姿势/动画帧 0**；ANI 帧矩阵以母版骨架（HOD 首块）为命名契约
- 骨骼变换统一公式（Body_d 修正 + 层级累乘 + Ry180°）对 HOD 与 ANI 通用（`02_RobotAndAnimation.md` §6.2）；动画关键帧已确认与绑定姿势同坐标系（§10 验证）

### 3. 复合资产定义（含动画）
- 当前 AssetTool 输出的是原子资产（dxmesh + .bone + 材质），动画剪辑（.anim）待 ANI 解析后接入
- 需要设计复合资产格式，整合骨骼动画 + mesh + 材质为完整的 Character 资产（`CharacterAsset.md`）

### 4. dxmesh 左手系验证
- 顶点和矩阵的转换逻辑已实现，但缺乏引擎端验证
- 需等到引擎蒙皮管线就绪后，配合测试确认结果

## 设计决策

- **武器/喷射口排除**：这些部件在 HOD 中保留完整骨骼数据，但 FBX/.x/dxmesh 导出时跳过。引擎通过挂载系统加载独立武器模型
- **root 排除**：root 是恒等矩阵的空骨架节点，FBX 中移除，子骨骼直接挂载到场景根节点
- **坐标系统**：引擎直接消费的资产（`.bone`/`.dxmesh`）为**左手系 Y-up**（`leftHanded=true`）；社区版工具 **Z-up（Z 上、Y 前）仅用于建模工具对比**，两者通过轴映射对应（详见 §2 坐标系说明）
