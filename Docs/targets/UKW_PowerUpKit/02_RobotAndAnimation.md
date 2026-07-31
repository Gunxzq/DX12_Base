# 机器人角色定义与动画格式

> 本章涵盖 Robo/ 目录下的角色定义文件（Script.spt）、动画（Script.ani）和颜色系统。

---

## 一、角色定义文件（Robo/Script.spt）

### 1.1 文件性质

- **编码**: Shift-JIS，文本格式，`'` 为注释
- **SPT 全称**: **Standard Parts Table** 或 **Special Parts Table**（来自 Windom Modding Wiki），即标准部件表
- **与地图 Script.spt 同名但不同**：地图 SPT 是场景组装指令；机器人 SPT 是**角色属性定义 + 部件表**
- **编译版本**: `Script.spt.a`（二进制加密/压缩格式，引擎加载的是此文件）
- **解密工具**: 社区有 `WindomXP File Decoder` / `Entranscode` 等加解密工具，可将 `.spt.a` 解密为可读的 `.spt`。某些杀毒软件可能误报，但无实际危害。
- **编辑方式**: 解密后用文本编辑器（记事本等）直接修改，修改后重新加密放回原目录。

### 1.2 指令集

#### 基础属性

```
@99 = 999999999.0;          ← 时间值（帧时长？）
Name=MS-98ﾊｽ;               ← 名称（Shift-JIS）
NameEng=MS-Type98;          ← 英文名
HP=1300;                    ← 生命值
Generator=1100;             ← 发电机出力
Energy=900;                 ← 能量值
Score=650;                  ← 分值
RestBody=4;                 ← 残机数
LockDist=80;                ← 锁定距离
```

#### AI 设定

```
AISetting(战斗倾向, 标准距离);
  ← 0=近接 〜 100=远距离

AIWeaponSetting(slot, 最小距离, 最大距离, 使用频率%, 能量消耗判定);
  ← slot: 0=shot, 1=近接, 2=sub1, 3=sub2, 4=sub3
```

#### 颜色设定（COLORSET）

```
COLORSET(配色编号, 材质编号, a, r, g, b);
  ← 配色编号: 1~9（玩家可选配色）
  ← 材质编号: 对应 .x 文件中 Material 的索引
  ← a,r,g,b: float (0~1)
```

同一配色可设定多个材质（多行），未指定的材质保留原色。

#### 推进器设定（BURNERSET）

```
BURNERSET(编号, 网格文件.x, 大小倍率, 方向);
  ← 方向: UP / DOWN
  ← 网格文件: Output01.x ~ Output08.x（喷口模型）
```

#### 武器与发射口设定（贴吧补充）

```
ATTACKARMSET(编号, 手臂网格.x);  ← 格斗武器/射击角度修正零件名称声明
GUNFILENAME(编号, 网格.x);       ← 持枪时才显示的零件名称声明
SWORDFILENAME(编号, 网格.x);     ← 持剑时才显示的零件名称声明

WEAPONPOINT(编号, 作为发射口的零件名称, 发射口性质);
  ← 编号: 0~4 = 手持武器发射口/挂点
  ← 编号: 18+ = 推进器喷口
  ← 性质: UP (普通发射口)
```

> 贴吧注释：**9L（即 WEAPONPOINT/ATTACKARMSET 等）是 ANI 所使用的部件和发射口/喷射口的代码设定**。不改 ANI 的话这些就没用。SPT 在属性定义之后还有对**调用模型的声明**（贴吧提及但未详细说明）。

#### 状态机（后续部分，由 ANI 和 Tail.dat 实现）

`Script.spt` 在属性定义之后是**状态机定义**，定义了机体的动作状态转换图：

- **状态**：待机/步行/冲刺/跳跃/攻击/受击/倒地/…
- **过渡条件**：按键输入/时间/碰撞/能量
- **每个状态引用**：动画帧段（.ani 中的帧范围）、特效、音效

> 状态机的详细格式**需要社区工具解码**，当前未作完整分析。

---

## 二、动画文件（Script.ani）

### 2.1 二进制结构

```
"AN2" 魔术头 (3 bytes)
        │
        ├── "Robo.hod\0" + padding (64 bytes)  ← 关联的骨架文件
        └── padding (0..255B)
        
"HD2" 骨骼通道头 (3 bytes)
        │
        ├── header_size (4B, 0x1E = 30)
        ├── ? (4B)
        ├── ? (4B)
        ├── ? (4B)
        ├── bone_name (64B)  ← 对应 HOD 中的部件名, e.g. "root.x\0"
        │
        ├── 每骨骼关键帧数据:
        │   ├── 帧数 (4B)
        │   ├── 帧率 (4B, float)
        │   └── 每帧:
        │       ├── 位置 (Vector3: 12B)
        │       └── 旋转 (Quaternion: 16B) 或 欧拉角
        │
        └── ...后续骨骼通道...
```

### 2.2 已知特征

- **"AN2"** = Animation 2？对应 DX9 时代的动画格式
- 每个骨骼通道关联 HOD 中定义的一个部件名
- 每帧存储 **位置 (float3) + 旋转 (quaternion)**，无缩放
- 骨骼层次从 HOD 的部件树中继承

### 2.3 社区工具（逆向/编辑）

| 工具 | 作者 | 功能 | 来源 |
|------|------|------|------|
| **WindomTranscoder** | LightningDragon | 文件加解密工具（解码 .x / .spt / .ani），支持 WindomXP 及 SeedMod | [GitHub](https://github.com/LightningDragon/WindomTranscoder) |
| **WindomXP File Decoder** | 社区 | SPT 文件加解密 | 社区论坛 |
| **Entranscode** | 社区 | 通用文件加解密（.x / .spt 等），部分杀毒可能误报 | Windom Extreme VS 论坛 |
| **ANI Tool V1.03** | ZerantoI / Aesreal | ANI 文件反汇编/编辑（将 Script.ani 分解为文件夹 + Tail.dat 文件） | Windom Extreme VS 论坛 |
| **Tail.dat Helper** | Zhmwwl | .dat 文件解密工具，将 Tail.dat 自动解密为可读文本并关联记事本 | Mediafire |
| **WindomXP Animations Editor** (β0.6) | Zhmwwl | HOD 编辑器 + 动画预览，支持 2.008 版 ANI 编辑/升级 | Mediafire |
| **Mecha Editor for Windom 2.008** | InfinitasImpetum | 编辑 2.008 版 ani、hod、script，含动画预览 | Mediafire |

#### ANI 编辑工作流（来自社区教程）

1. 用 **ANI Tool** 将 `Script.ani` 分解为多个文件夹（每个动作一个文件夹，内含 `Tail.dat` 和 `.hod` 文件）
2. 各文件夹序号对应不同动作（如 folder 60 = subattack 3）
3. 用十六进制编辑器（HxD）打开 `Tail.dat`，编辑攻击参数：
   - `RunProc2(type, subType, partIndex, cost)` — 攻击动作调用
   - `WeaponAttack(type, subType, partIndex, cost)` — 武器攻击定义
   - type: 1=小光束, 3=小导弹, 4=导弹舱, 24=投射物, 57=剑攻击
   - partIndex: 对应 SPT 中 WEAPONPOINT 定义的发射口编号
4. 编辑完成后用 ANI Tool 重新打包为 `Script.ani`
5. 关键：保持工作目录在桌面，**务必备份原始文件**

> 注意事项：
> - PowerUp Kit (PUK) 版本的 ANI 格式比原始版更复杂，部分工具尚未完全支持
> - 所有工具需通过 **Microsoft Applocale** 以简体中文区域启动（Shift-JIS 编码）
> - `.hod` 和 `Tail.dat` 一律用十六进制编辑器打开，避免文本编辑器编码破坏

---

## 三、机体部件网格（.x）

### 3.1 存储约定

机体每个部件是一个独立的 `.x` 文件，包含：

- **Mesh** — 顶点位置、三角面
- **MeshNormals** — 顶点法线
- **MeshTextureCoords** — UV 坐标
- **Material** — 漫反射颜色 + 光泽度 + 纹理文件名

### 3.2 命名约定

| 后缀 | 含义 |
|------|------|
| `Body.x` | 身体（正常） |
| `Body_d.x` | 身体（受损，纹理/模型变化） |
| `arm1.x` | 左臂 |
| `arm1_r.x` | 右臂 (`_r` = right) |
| `leg1.x` | 左腿 |
| `leg1_r.x` | 右腿 |
| `Head_.x` | 头部（非标准变体，下划线后缀） |
| `root_Fannel.x` | 浮游炮组件（Funnel） |

### 3.3 材质索引与 COLORSET 的对应

模型中的 Material 按出现顺序编号（0-based），`COLORSET(id, matIndex, a,r,g,b)` 中的 `matIndex` 即对应此编号，实现换色功能。

典型材质分配：
- `mat 0`: 主装甲色
- `mat 1`: 副装甲色
- `mat 3`: 关节/内构色
- `mat 4`: 细节/点缀色

---

## 四、贴图与 UI

| 文件 | 用途 |
|------|------|
| `face.png` | 角色头像（战斗 UI） |
| `face2.png` | 角色头像（对话） |
| `select.png` | 角色选择画面图标 |
| `Select2.png` | 选择画面图标（备选） |
| `tex.png` | 机体纹理图集（若有） |
| `IMG0082.png` | 其他 UI 元素 |

`charaselect.sdt` 和 `charaselect_eng/jp.sdt` 是角色选择画面描述文件（二进制）。

---

## 五、机库展示（hangar.hod）

`hangar.hod` 结构与 `Robo.hod` 相同，但部件的变换矩阵经过调整，使机体在机库场景中呈现展示姿态（如停放在地面而非战斗站立）。

---

## 六、DX12 管线映射（2026-07-30 更新）

### 6.1 资产管线

```
HOD → hod.json (骨架树 + 绑定姿势矩阵 = 动画帧 0)
         ↓
Script.spt → ActorDefinition (HP/AI/武器插槽/配色)
         ↓
.ani → AnimationClip (StorageBuffer: 每骨骼 keyframe, 暂缓)
         ↓
COLORSET → MaterialOverride (每材质颜色常数 PushConstant)
         ↓
.x Body/arm/leg → 合并为单一 .dxmesh
                        ├─ DxMeshSkinnedVertex (每顶点 boneIndex + weight)
                        └─ SubMesh 表 (每个 .x 部件 = 一个 SubMesh)
```

### 6.2 骨骼变换统一公式

HOD 绑定姿势与 ANI 动画关键帧共用同一套骨骼变换公式：

```
输入: 每骨骼局部矩阵 (HOD 原始矩阵 或 ANI 插值矩阵)
  ↓
[1] Body_d 偏移修正: if (name == "Body_d.x") local.m[13] -= 1.30f
  ↓
[2] 层级累乘: boneWorld[bi] = local × boneWorld[parent]  (行向量)
  ↓
[3] Ry(180°) 翻转: bw.row0 = -bw.row0;  bw.row2 = -bw.row2
  ↓
输出: boneWorld 矩阵 (Y-up 世界空间)
```

| 数据源 | 说明 | 状态 |
|:-------|:------|:------|
| **HOD** (绑定姿势) | 每个 entry 的 4×4 矩阵经上述公式 → 绑定姿势 boneWorld | ✅ HODParser 已实现 |
| **ANI** (动作帧) | 每骨骼 pos+rot 关键帧插值 → 局部矩阵 → 同上公式 → 动画 boneWorld | ❌ 暂缓解析 |

> 骨骼蒙皮的细节见 `Basic.txt` 着色器中的 `vs_2_0 SkinVS` — 使用 `D3DCOLORtoUBYTE4` 解码 `BLENDINDICES`，支持最多 4 个骨骼权重，最多 26 个骨骼矩阵。

### 6.3 .x 嵌套层级导出（2026-07-31 更新）

导出包含完整骨骼树和正确局部矩阵的 `.x` 文件，采用两阶段处理：

**第一阶段：世界矩阵计算**（`boneWorld`）

```
HOD 原始矩阵 → Body_d 偏移 → 层级累乘 → Ry180° → boneWorld (Y-up 世界空间)
```

**第二阶段：嵌套层级 + 局部矩阵推导**

```
每骨骼的局部矩阵:
  根骨骼: localMat = boneWorld[根]
  子骨骼: localMat = boneWorld[子] × inverse(boneWorld[父])  (行主序 post-multiply)
```

**LR 交换**：交换 `_r` 后缀部件的 name 和 mesh 数据（不交换 boneIndex/矩阵），使右部件的网格和名字出现在正确位置：

```
swapLR("arm1", "arm1_r"); swapLR("leg1", "leg1_r");
swapLR("arm2", "arm2_r"); swapLR("leg2", "leg2_r");
swapLR("Hand", "Hand_r"); swapLR("leg3", "leg3_r");
```

**输出文件**：
- `Robo_nested.x`：29 骨骼嵌套层级，局部矩阵，正确网格

### 6.4 顶点格式：DxMeshSkinnedVertex 刚性绑定

合并网格时，顶点**不解烘焙到世界空间**，保持局部坐标，改为 `DxMeshSkinnedVertex` 格式：

```
结构: float3 position
      float3 tangentU
      float3 normal
      float2 texC
      float4 boneWeights   ← {1.0f, 0, 0, 0}
      uint8_t boneIndices[4] ← {subMeshBoneIndex, 0, 0, 0}
      total = 64 bytes
```

- 每个部件的所有顶点属于同一骨骼（刚性绑定），`boneWeights[0]=1.0`
- 运行时由 `boneWorld[boneIndex]` 驱动顶点变换
- SubMesh 与骨骼解耦：SubMesh 服务于材质槽，不隐含骨骼映射

---

## 七、骨架数据对接（2026-07-30 更新）

### 7.1 hod.json → SkeletonManager

HODParser 输出的 JSON 骨架文件（.hod.json）需要被引擎 `SkeletonManager` 加载。JSON 结构如下：

```json
{
    "filepath": "Robo/KD-03/Robo.hod",
    "bones": [
        {
            "name": "Body_d.x",
            "parentIndex": -1,
            "children": [1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14],
            "position": [0, 1.362, 0],
            "rotation": [0, 0, 0, 1],
            "scale": [1, 1, 1]
        },
        {
            "name": "Body.x",
            "parentIndex": 0,
            "children": [],
            "position": [0, 0, 0],
            "rotation": [0, 0, 0, 1],
            "scale": [1, 1, 1]
        },
        {
            "name": "Head.x",
            "parentIndex": 0,
            "children": [],
            "position": [0, 2.5, 0],
            "rotation": [0, 0, 0, 1],
            "scale": [1, 1, 1]
        }
    ]
}
```

### 7.2 HOD 作为动画帧 0

HOD 的每个 entry 定义了一个部件（骨骼）及其 4×4 变换矩阵。这个矩阵经 Body_d 修正 → 层级累乘 → Ry(180°) 翻转后，得到**绑定姿势的 boneWorld**。在动画系统中，HOD 相当于第 0 帧：

```
HOD entries (名称 + 矩阵 + A/B)
  → BuildHierarchy() 通过 A/B 栈算法构建父子关系
  → 每骨骼 local 矩阵 = entry.transform (原始 Z-up)
  → 应用修正公式 (Body_d offset + 层级累乘 + Ry(180°))
  → 输出 boneWorld[0..N] = 绑定姿势 (Y-up，动画帧 0)
```

ANI 的 `HD2` 骨骼通道名与 HOD 部件名一一对应，插值后应用**同一套公式**得到动画帧的 boneWorld。

### 7.3 骨架与网格的关联

```
合并时 SubMesh 顺序 = .hod 中部件顺序（筛选后）
即 parts[0] = Body.x → SubMesh[0], parts[1] = Head.x → SubMesh[1], ...
```

SubMesh 与骨骼**无结构化绑定**。SubMesh 服务于材质槽，骨骼服务于顶点变换，两者是正交关系。合并输出的 .dxmesh 使用 `DxMeshSkinnedVertex`，每顶点记录其所属的 boneIndex：

```
parts[i] 的所有顶点:
  boneIndices = {boneIndexOf(parts[i]), 0, 0, 0}
  boneWeights = {1.0f, 0, 0, 0}
```

运行时 `SkinnedRenderer` 通过 GPU 骨骼缓冲区驱动全部顶点，不关心 SubMesh 边界。

### 7.4 SkinnedComponent 的构建

场景加载时，从 `scene.json` 的 `"skinned"` 块获取骨架引用：

```cpp
// SceneConstructor 中
if (eDesc.skinned) {
    auto skeletonHandle = skeletonMgr->LoadFromJSON(eDesc.skinned->skeleton);
    registry->AddComponent<ECS::SkinnedComponent>(entity, skeletonHandle);
}
```

`SkinnedComponent` 目前已存在（`ECS/Core/Components/Animation.h`），含 `skeletonHandle` 字段。蒙皮渲染路径（`SkinnedRenderItemBuilder` + `SkinnedRenderer`）已在引擎中实现。

### 7.5 当前状态与缺口

| 环节 | 状态 | 说明 |
|:-----|:------|:------|
| HOD → hod.json | ✅ HODParser 完整 | 已有 TRS 分解 + JSON 输出 |
| hod.json → SkeletonHandle | ❌ 引擎侧未实现 | `SkeletonManager` 缺少 `LoadFromJSON` 接口 |
| SkinnedComponent 创建 | ⚠️ 框架已有 | `SceneConstructor` 中需要补充 skinned 分支 |
| 网格合并 + SubMesh | ✅ `DxMeshSkinnedVertex` 刚性绑定 | AssetTool 输出 skinned 格式，不解烘焙 |
| ANI 动画解析 | ❌ 暂缓 | 不阻塞当前工作 |

> **网格合并变更**（2026-07-30）：合并管线改为保留局部坐标，输出 `DxMeshSkinnedVertex` 每顶点含 `boneIndex`+`boneWeight`，不解烘焙到世界空间。旧版烘焙式输出仅用于 DE 验证（`.x` 层级导出）。

### 7.6 骨架与部件的关联

HOD 部件名与 `.x` 文件名（去掉扩展名）一一对应。合并输出时：

- `parts[]` 顺序 = HOD 骨骼顺序（筛选后）
- 每个 part 的顶点统一挂接到其 `boneIndex`（刚性绑定）
- `Body_d.x` 保留在骨架中用于后期受损特效，但不参与网格合并
