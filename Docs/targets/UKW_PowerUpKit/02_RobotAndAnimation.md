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

### 2.1 源文件二进制结构（2026-07-31 实测修正）

> **勘误**：旧版文档记载 `AN2`/`HD2` 魔术头，经实测 `KD-03/Script.ani`（3.5MB，2009 原版）**无 `AN2`/`HD2` 魔术**（全文检索 0 次），实际结构为：

```
0x000000: "ANIRobo.hod\0" + padding      ← 64B 文件名区（关联的骨架母版名）
0x000103: [块 #0]   size=10137           ← 母版骨骼（ANIRobo.hod）
0x002888: "robo_stand.hod\0" + padding   ← 帧文件名区（20B 对齐）
0x00289C: [块 #1]   size=9877            ← 动作帧 1（组 01 第一帧）
0x004F1D: "robo_stand.hod\0" + padding
0x004F31: [块 #2]   size=9877            ← 动作帧 2
... 共 348 个块（按 .hod 文件名计数，含母版）...
0x35DC71: "robo_fly_stop01.hod\0"
0x35DC80: [块 #347] size=15413           ← 末尾块（含尾部数据）
```

> **勘误补充（2026-07-31）**：`AN2`/`HD2` 并非不存在，而是 **PUK（2.008 版）专属格式**。1.008 原版无 AN2/HD2；PowerUp Kit 版的 `Script.ani` 头部为 `AN2Robo.hod\0`，块魔术为 `HD2`（见下方 §2.4 PUK 版差异）。

**块边界 = `.hod` 文件名后缀**（348 个，与拆解产物 348 个文件逐一对齐）；`HOD` 魔术只是块内数据头，用 `HOD` 子串计数会得到 350（2 个误报来自数据内部字节巧合）。

**源文件整体布局 = 母版 + 动画组序列（2026-07-31 实测）**：

```
[文件名头 "ANIRobo.hod" + padding]
[母版块 HOD]                          ← 冗余：每帧 HOD 自带完整骨骼，可跳过
[组 01: 3×HOD 帧块][Tail 明文状态机]  ← Tail 紧跟在组内最后一帧 HOD 数据(+9847B)之后
[组 02: 9×HOD 帧块][Tail 明文状态机]
... 共 65 组，Tail 均为明文 SPT（Shift-JIS），IF 块以 ENDIF; 闭合
```

**Tail 段内结构（明文 SPT，段间 12B 标记）**：

```
[段1 脚本: '日文注释 IF(@int[151],==,0); Move=(...); BURNER(0-5); ENDIF;]
[12B 段间标记: 段时长/速度/脚本大小]
[段2 脚本: ...]
[12B 段间标记]
...
```

**Tail.dat 16B 头 ↔ Tail.txt 对应关系（2026-07-31 实测）**：

| Tail.dat 字节 | Tail.txt 显示 | 含义 |
|---|---|---|
| `01 00 00 00` | — | 版本=1 |
| `08 00 00 00` | `8,` | 时间值（段落时长） |
| `CD CC CC 3D` (0.1f) | `,1` | 速度值 |
| `E0 00 00 00` | — | 脚本数据大小 |
| 明文 SPT 脚本 | `#####Begin#####`~`#####End#####` 之间 | 脚本主体逐字一致 |

> `---NNN---`（段落编号）、`#####Begin#####`/`#####End#####`（边界）、`===000===`（文件尾）均为拆解器添加的标记，Tail.dat 本体只有 16B 头 + 明文脚本。源文件中 Tail 前紧邻的 63B 数值区是 HOD 帧矩阵尾部，非 Tail 组成部分。

**解析策略（2026-07-31 定案，标记法 + 母版驱动，无固定步长）**：
- 与 `.mpd` 地图一样采用**标记模式**，固定步长不可行（部件数随机体变化：30/31/52/75...，帧数据大小 = 7+N×328 或 N×179-8+19，须从块头读部件数推导，不可写死）
- **帧边界标记**：扫描 `.hod` 文件名（1.008/PUK 标准）或 `HD2` 魔术（无 `.hod` 块标记变体，如 `AN2a.hod` 机体的 KD-04_4）
- **帧数据结束 = 下一帧名字起点**（= 下一帧 `.hod` 位置 − 名字长度），组内帧精确
- **组尾判定 = Tail 明文特征**（`IF(@` + 16B 头：版本 1..10 / 时间 / 速度 / 脚本大小）在 [块数据, 名字起点) 区间定位；**空 Tail 组**（Tail 全零无明文）用"部件数 × 每部件大小"推算帧数据结束兜底
- **Tail 解析**：16B 头 + 段结构推进（段1 用 16B 头时间/速度/大小，段2..n 各带 12B 段头）→ 跳零到 Note 起点
- 无需依赖动作名称（Shift-JIS 日文难解码），也无需 Head.txt（那是拆解产物的统计信息，源文件不包含）
- 目标：按组提取"帧数据 + Tail 状态机脚本"即可，不必全量解码每个字段

**母版驱动（2026-07-31 确认）**：母版块（HD2 类型=0 / HOD 首块）包含**完整骨架信息**——部件名 + A/B 层级 + 变换数据，可完全替代外部 `Robo.hod` 作为骨骼来源；帧块（HD2 类型=1）不含部件名（仅矩阵数据），以母版为基准索引对应。

**与拆解产物一致性验证（2026-07-31 通过）**：
- HOD 帧数据：拆解 348 文件（127 唯一内容）的 md5 全部可在源文件 HOD 块中找到 ✅（源文件含 128 唯一，多出 1 个为母版块）
- Tail 状态机：65 个 Tail.dat 全部作为连续字节存在于源文件 ✅
- 名称/路径不必一致（源文件内组间帧同名，`00X_` 前缀为拆解器按序添加），内容级一致即证明解析可得全部数据

**HOD 块内部**（与拆解后的独立 `.hod` 文件逐字节同构）：

```
"HOD" 魔术 (3B) + 0x1E (30 = 部件数)
  │
  ├── 部件名区 (64B/部件): "root.x\0" ...  ← 对应 .x 部件名
  ├── A/B 层级数据: A=部件等级, B=子部件数量
  └── 部件局部 4×4 矩阵 (固定父子结构)
```

### 2.2 已知特征

- **源文件 = 文件名头 + 连续 HOD 块序列**（1.008 原版；PUK 2.008 版为 HD2 块，见 §2.4）
- 每个 HOD 块 = 一帧的**部件局部矩阵快照**（固定父子结构，无传统骨骼概念）
- 块 #0 为母版（10137B），动作帧块 9877B——**差异 = 文件名区长度**：动作帧文件名区 30B（`名字\0` 15B + `CD` 填充 15B，20B 对齐后置 HOD），母版文件名区更大（248B+ 后才见 HOD）。HOD 数据本体恒为 9847B
- **块计数 = 348**（按 `.hod` 文件名后缀匹配，含母版），与拆解产物 348 个文件（347 帧 + 母版）**完全一致** ✅（用 `HOD` 子串计数会得 350，其中 2 个为数据内部字节巧合的误报）
- **母版冗余（2026-07-31 确认）**：每个动作帧 HOD 自带**完整骨骼信息**（部件名/A/B/矩阵与母版逐字节一致，实测母版 vs 帧1 vs 帧2 部件名 29 个全同）。解析器不需要母版定位骨骼基准，任意动作帧即可获得骨骼树；以母版为基准亦可（社区惯例）
- **块头字段**：`HOD` + `1E000000`(30=部件数) + `00000000` + `01000000`（后两个为固定未知字段，350 块全同，含义待逆向）
- **Tail.dat 为明文**：16B 头（版本=1 / 时间值 / 速度值 0.1f / 脚本数据大小）+ 明文 SPT 脚本（Shift-JIS 编码，无需解密）

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

### 2.4 PUK 版（2.008）差异与母版驱动解析（2026-07-31 实测确认）

> **AN2/HD2 正是 PUK 版专属格式**：`AN2`/`HD2` 并非不存在，而是 1.008 原版之外的 PowerUp Kit（2.008）版本才有的魔术。社区文档记载无误，此前"无 AN2/HD2"的勘误仅适用于 1.008 原版。

**PUK 版与 1.008 原版对比（KD-03 等实测）**：

| 项 | 1.008 原版 | PUK 2.008 版 |
|---|---|---|
| 文件头 | `ANIRobo.hod\0` | `AN2Robo.hod\0`（母版名随机体变化，如 `AN2a.hod`） |
| 块魔术 | `HOD` | `HD2` |
| 块头 | `HOD` + 部件数 + 0 + 1（15B） | `HD2` + **类型(0=母版/1=帧)** + 部件数 + 0 + 1（19B） |
| 块位置 | `.hod` + 20B | `.hod` + 4B |
| 部件数偏移 | 魔术后 +3 | 魔术后 +7 |
| 帧数据大小 | 7 + N×328 | **N×179 - 8 + 19**（实测 N=30/31/52/75 全部吻合） |
| 每部件 | 328B（256名+64矩阵+8AB） | 179B（171 数据 + 8 AB，**帧块不含部件名**） |
| 母版部件 | 含部件名（root.x @0x0F） | 含部件名（root.x @0x13），**399B/部件 = 256 名 + 135 数据 + 8 AB** |

**母版 = 完整骨架来源（无需外部 HOD）**：
- PUK 母版（HD2 类型=0）包含全部部件名（root.x → Output06.x）+ A/B 层级（root A=1B=3、Body_d A=2B=5，与 HOD 逐对一致）+ 变换数据
- 母版 399B/部件布局在 KD-03/04_2/05/06_2 四机体通用（部件数 30/31/52/75 均成立）
- 因此骨架/骨骼树可直接从 ANI 母版解析，**不依赖同目录 `Robo.hod`**

**变体：无 `.hod` 块标记的机体（如 KD-04_4，头部 `AN2a.hod`）**：
- 帧间**无 `.hod` 文件名**（全文仅头部 1 处 + 中部 2 处巧合），帧边界 = `HD2` 魔术本身
- 名字区 = `[uint16 长度][Shift-JIS 名字]` 紧邻 HD2 前（如 `12 00` + 18B 名字）
- 帧数据结束 = 下一 HD2 位置 − 名字区长度（与公式 N×179-8+19 精确吻合，实测 KD-04_4：51 部件 → 9140B，289 帧 74 组）
- 检测：HD2 数量 ≥ 8 且 `.hod` 数量×4 < HD2 数量 → 变体模式，直接扫描 HD2 魔术定位帧块

**Tail 段完全同构**：PUK 版 Tail 与 1.008 一致（16B 头 + 明文 SPT，`IF(@`/`ENDIF;`），193 个明文 Tail 全部位于 HD2 块间，Tail 解析逻辑直接复用。

**25 机体全量解析结果（2026-07-31 AssetTool 实测）**：成功 25 / 失败 0；组数随机体 2~82 组不等（KD-03=66、KD-04_4=74、KD-05=76、KD-08_7_2=54...）。

**ANI 母版管线与步骤划分（2026-07-31 定案）**：
- **目标**：导入机体的合并部件功能改为 ANI 母版驱动——解析母版拿骨骼 → 合并部件 → 输出母版对应 x/fbx/bone → 全部动画数据 + Tail 按组分隔输出到单一 TXT（不分文件夹）→ 最终得到**含动画信息的 FBX + 状态机 Tail 文本**
- **已实现**：
  - `ANIParser::GetMaster()`：解析母版块（HD2 类型=0 / HOD 首块）→ `ANIMaster`（部件名 + A/B 层级，root 在 index 0）
  - `RobotMerger::MergeFromANI(aniPath, outputDir)`：母版驱动合并入口——解析 ANI 母版 → 定位同目录 `Robo.hod`（绑定矩阵）→ 校验母版/HOD 部件数 → 复用 HOD 合并管线输出 x/fbx/bone（bone 同样输出）
- **绑定矩阵来源（决策）**：PUK 母版数据区（135B/部件）经实测为 **TRS 编码而非 4×4 矩阵**（Body_d 数据区 pos=(0.15,0.41,0.07) vs HOD 绑定矩阵平移 (0,1.3624,0) 完全不同），故矩阵**暂用同目录 Robo.hod**（HODParser 已验证）；母版数据区 TRS 格式待逆向后可彻底脱离 HOD
- **下一任务（B2.5）**：动画帧矩阵 + Tail 状态机按动画组分隔输出到同一 TXT（不分文件夹）

### 2.5 动画帧 → FBX 转换链路（B2.5，2026-07-31 设计）

**动画数据来源（拆解帧 = 标准 HOD，已实测确认）**：
- **1.008 原版**：拆解出的帧文件就是**标准 HOD**（9847B，`0x0F + 30×328 - 8`，HODParser 直接可解析）✅ —— **无需任何逆向**
- **PUK 2.008**：帧块为 HD2 紧凑格式（19B 头 + 每部件 179B = 171B TRS 数据 + 8B A/B），`ANIParser` 已按此结构拆解成功（A/B @ +171 提取、部件数校验全过）——结构已逆向，数据区 TRS 语义与母版一致（position/rotation/scale），**可作为 TRS 原样传入**
- 实测 walk 组 8 帧 Body_d：`f[1]`（Y 位移 0.0436→0.0698→-0.0436...）、`f[8]`（身体高度 1.4022→1.3241→1.3999...）随帧变化；root 恒定——**跨帧差异即动画轨迹** ✅

**转换链路（HOD 帧 → FBX aiNodeAnim，HODParser 直解，无需逆向 TRS）**：

```
ANI 拆解帧（1.008: 标准 HOD 9847B / PUK: HD2 帧块）
  │ ① HODParser 解析帧数据 → 每部件 4×4 矩阵（1.008 直接喂 HODParser::Parse）
  │    PUK 帧块由 ANIParser 按已逆向结构拆出每部件 TRS → 组装 4×4
  │ ② 层级累乘 → 世界矩阵 boneWorld（复用 §6.2 公式：Body_d 修正 + 累乘 + Ry180°）
  │ ③ 世界 → 局部：fbxLocal = world[child] × inverse(world[parent])（与静态 FBX 导出一致）
  │ ④ TRS 分解（XMMatrixDecompose，复用 .bone 导出逻辑）
  ▼
每骨骼生成 aiNodeAnim：
  mPositionKeys[]（每帧 position）
  mRotationKeys[]（每帧 quaternion）
  mScalingKeys[]（每帧 scale）
  ▼
每动画组 = 一个 aiAnimation（含全部骨骼通道，mName = 组名如 "walk01"）
  ▼
scene->mAnimations += aiAnimation → Assimp::Exporter 导出 FBX（含动画）
```

**关键处理点（B2.5 实施时确认）**：
1. ~~帧块 TRS 布局逆向~~ **无需逆向**：1.008 帧 = 标准 HOD（HODParser 直解）；PUK 帧块结构已逆向，TRS 语义与母版一致可直接使用
2. **时间轴**：每帧时长来源——Tail 16B 头 time/speed 字段（段落时长）或固定帧率（如 30fps），需与社区拆解对照
3. **骨骼命名对齐**：aiNodeAnim 通道名 = 骨骼名（含 .x 后缀），与 aiNode 完全一致（`RobotMerger` 6a 已按 `hod.bones[bi].name` 命名）
4. **通道过滤**：与静态 FBX 一致，只为可渲染骨骼生成动画通道（排除武器/喷射口/root）
5. **绑定姿势基准**：FBX 节点 mTransformation 已是绑定姿势；动画关键帧应为**相对绑定姿势的增量**还是绝对 TRS，需以 Blender 导入验证为准
6. **多动画组导出**：一个 FBX 可含多个 aiAnimation（walk01/walk02/attack...），或按组分文件导出，取决于引擎 AnimLoader 接入方式

**PUK 输出帧块与社区工具兼容性（2026-07-31 确认）**：
- 社区工具（ANI Tool V1.03 等）按 **1.008 HOD 格式**解析：期望 `HOD` 魔术 + 每帧含部件名区（328B/部件）
- PUK 版源文件动作帧块**不含部件名**（部件名仅存于母版 HD2 类型=0），故 AssetTool 按组导出的 HD2 帧块（`HD2` 魔术 + 19B 头 + N×179-8 数据，5383B）**社区工具无法打开**——这是格式差异，非数据缺失
- **矩阵信息完整**：帧块内含全部部件 4×4 局部矩阵 + A/B 层级（实测 A/B 序列与母版逐对一致：root A=1B=3、Body_d A=2B=5...）
- **帧名一致**：`00X_robo_stand.hod` 等按序前缀与社区拆解产物逐文件名匹配（源文件 `.hod` 文件名标记保留）
- 结论：PUK 帧块的"部件名→矩阵数据"对应关系需通过**母版**（HD2 类型=0 的 30/31/52/75 部件名列表）重建；引擎侧可直接消费帧矩阵，无需社区工具中转

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
.ani → AnimationClip (每骨骼 keyframe; 🔜 解析器下一会话实现)
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
| **HOD** (绑定姿势) | 每个 entry 的 4×4 矩阵经上述公式 → 绑定姿势 boneWorld（= 动画帧 0 / 初始姿势） | ✅ HODParser 已实现 |
| **ANI** (动作帧) | HOD 块部件局部矩阵 → 同上公式 → 动画 boneWorld | 🔜 解析器下一会话实现；先解析（文件名头 + HOD 块 → 每帧部件局部矩阵），`.anim` 现代化资产化为下一步 |

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

ANI 的 HOD 块部件名与母版 HOD 部件名一一对应，插值后应用**同一套公式**得到动画帧的 boneWorld。

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
| HOD → hod.json | ✅ HODParser 完整 | 已有 TRS 分解 + JSON 输出（转置 bug 已修复，position 恢复非零） |
| hod.json → SkeletonHandle | ✅ 已实现 | `SkeletonManager::LoadFromJSON` + `SkeletonLoader::ParseBoneFile`（2026-07-31） |
| .bone 资产导出 | ✅ 已验证 | RobotMerger 输出 Y-up 局部矩阵，与 nested.x 逐位一致（见快照） |
| SkinnedComponent 创建 | ⚠️ 框架已有 | `SceneConstructor` 中需要补充 skinned 分支 |
| 网格合并 + SubMesh | ✅ `DxMeshSkinnedVertex` 刚性绑定 | AssetTool 输出 skinned 格式，不解烘焙 |
| **ANI 动画解析** | 🔜 **下一会话第一任务** | 先实现解析器（HOD 块序列 → 每帧部件局部矩阵）；`.anim` 现代化资产化为下一步 |

> **网格合并变更**（2026-07-30）：合并管线改为保留局部坐标，输出 `DxMeshSkinnedVertex` 每顶点含 `boneIndex`+`boneWeight`，不解烘焙到世界空间。旧版烘焙式输出仅用于 DE 验证（`.x` 层级导出）。

### 7.6 骨架与部件的关联

HOD 部件名与 `.x` 文件名（去掉扩展名）一一对应。合并输出时：

- `parts[]` 顺序 = HOD 骨骼顺序（筛选后）
- 每个 part 的顶点统一挂接到其 `boneIndex`（刚性绑定）
- `Body_d.x` 保留在骨架中用于后期受损特效，但不参与网格合并

---

## 八、运行时 IK 观察与分析（2026-08-01）

> **背景**：UKW 是类 EXVS 的射击游戏，脚步着地与手部瞄准为目标需求。原版应通过运行时 IK 实现（枪口实时对准、脚踩斜坡贴合），但 ANI 烘焙帧只记录基础动作，IK 修正不落盘，**ANI 数据中看不出来，需要引擎自行实现**。

### 8.1 骨架结构确认：链式 + 局部矩阵

HOD 骨架为**链式层级**，每骨骼存**相对父的局部矩阵**（非锚点扁平结构）。真实骨架树见 `ANI_Parsing_Results.md`（注意：`01_AssetFormatOverview.md` §4.2 的部件树为简化展示，将四肢画成平级，实际 A/B 层级为链式）：

```
Body.x
├── arm1.x → arm2.x → Hand.x          ← 左臂 3 节链（肩→肘→腕）
├── arm1_r.x → arm2_r.x → Hand_r.x    ← 右臂 3 节链
├── leg1.x → leg2.x → leg3.x          ← 左腿 3 节链（髋→膝→踝）
├── leg1_r.x → leg2_r.x → leg3_r.x    ← 右腿 3 节链
```

- 局部矩阵公式（§6.2）：`boneWorld[bi] = local × boneWorld[parent]`（Body_d -1.30 修正 + 层级累乘 + Ry180°），已在 nested.x / .bone / ani2frames 三处验证一致
- **骨骼 = 可见部件**：UKW 极老，无独立骨架资产，HOD 拼接部件（实体 `.x` 模型）本身就是骨骼；排除武装（gun/sword/Shield/Weapon_point 等）后，剩余部件即全部可见骨骼 → **IK 修改骨骼矩阵 = 直接驱动可见部件，天然直观**

### 8.2 关节自由度观察：必然多轴，不能锁轴

对 KD-03/04/05/06 四台机体 `{stem}_anim_frames.txt`（逐帧局部矩阵）做旋转轴稳定性分析（增量旋转 ΔR = R_f × R_0^T 提取轴角，按组统计轴偏差 <20° 为稳定组）：

| 关节 | KD-03 | KD-04 | KD-05 | 结论 |
|:--|:--|:--|:--|:--|
| leg2（膝） | 45 稳 / 0 不稳 | 23 稳 / 22 不稳 | 28 稳 / 24 不稳 | 同是膝，KD-03 接近铰链，KD-04/05 明显多轴 |
| arm2（肘） | 31 / 14 | 29 / 16 | 33 / 2 | 大部分组稳定，受击/挥剑组轴漂移大 |
| arm1（肩） | 27 / 16 | 27 / 16 | 22 / 26 | 基本为多轴（球窝） |
| Hand | 10 稳 / 0 不稳 | 10 / 0 | 0 / 0 | 数据里手几乎不独立旋转（刚性） |

**结论**：
1. **关节必然多轴**（从 EXVS 目标与实测均确认），同一关节在不同机体/动画组间旋转轴变化大，**不能按单轴铰链锁轴设计** IK
2. IK 求解器应选 **FABRIK / CCD 无轴约束迭代求解**，或两段 IK 不锁轴；锁轴只对 KD-03 这类部分机体成立
3. 轴稳定性统计本身无需再做——多轴为既定前提

### 8.3 机体间骨骼命名与结构不统一（IK 链定义必须母版驱动）

四台机体实际骨骼名对比（`{stem}_anim_frames.txt` 提取）：

```
KD-03/04:  arm1/arm2/Hand/leg1/leg2/leg3（小写，15 骨骼）
KD-05:     arm1/arm2/Hand/leg1~3 + MP/MP_r/Shield/legG（28 骨骼）
KD-06:     Arm1/Arm2/Arm3（大写！）+ Wing1~3/W_Tail/BackW/LegG（43 骨骼）
```

- KD-06 用**大写命名**且多 Arm3（前臂分段）、Wing/Tail 结构
- **IK 链定义不能硬编码骨骼名**：引擎侧必须以**母版骨架**（`ANIParser::GetMaster()` 的部件名 + A/B 层级）动态识别 `arm1→arm2→Hand` / `Arm1→Arm2→Arm3→Hand` 这类链，而非写死字符串

### 8.4 Blender 骨骼视觉不连续（已确认不改数据）

- **现象**：FBX 导入 Blender 后骨骼显示为互不连接的短棒，层级树与动画均正确
- **根因**：HOD 矩阵描述的是**部件变换**（位置/朝向），非**关节链几何**（head/tail）；FBX 骨骼节点只有矩阵，Blender 全靠矩阵推导 head（=平移）与 tail（=沿局部轴固定长度），部件间有物理间隙 → 视觉断开。**与数据正确性无关**
- **已实测**：改动骨骼（head/tail）会导致动画效果异常——head/tail 与骨骼矩阵耦合，改了就破坏蒙皮与动画。**结论：不动骨骼数据**
- **处理**：Blender 侧纯显示调整（Display As: Stick/Wire + 调大 Display Size + In Front）过渡；重建 rig（新骨架 + 权重迁移）为一次性工程，仅在确认需要长期 DCC 动画工作时立项

### 8.5 IK 落地路径（B 方案：AssetTool 离线验证先行）

| 方案 | 内容 | 依赖 |
|:--|:--|:--|
| **B（已实现，2026-08-01）** | 在 AssetTool/CLI 侧实现 FABRIK 求解器 + 母版驱动链定义，离线验证"脚贴地/手瞄准"求解正确性（不依赖引擎） | ✅ 已完成并验证通过 |
| A | 先补引擎蒙皮管线三段缺口（SkeletonManager hod.json 加载 / GPU 骨骼缓冲 / 蒙皮着色器绑定）再挂 IK | 管线缺口未清，IK 无法端到端验证 |

- B 与 A 互不阻塞：B 验证求解器数学正确性，A 推进引擎侧数据通路，两者并行
- 引擎运行时 IK（脚步着地 + 手部瞄准）最终形态：FABRIK/CCD 多轴求解 + 动画/IK 混合权重（待定）

#### B 方案实现（2026-08-01）✅

**新增文件**：
- `AssetTool/Core/IKSolver.h/.cpp` — FABRIK 多轴无约束求解器：
  - `FindChains(hod)`：母版驱动链识别（名字含 arm/leg 大小写不敏感 + 层级追踪，排除武装/辅助节点）
  - `SolveFABRIK(joints, target)`：纯位置前后向迭代，段长恒定，目标不可达时整链拉直
  - `ApplyPositionsToLocal(chain, hod)`：求解后关节位置 → 每骨骼局部矩阵（保持原旋转仅平移）
- `AssetTool/CLI/main.cpp` — 新增 `ani2ik <Script.ani> <output_dir> [stem]` 命令，输出 `{stem}_ik_report.txt`

**CMake**：AssetTool 源文件为**显式列表**（非 GLOB），新增 IKSolver 需在 `CMakeLists.txt` 的 `ASSET_TOOL_SHARED_SOURCES` 手动登记（初次构建 LNK2019 根因，已修复）。

**验证结果（KD-03 小写 / KD-06 大写）**：
- 链识别：KD-03 识别 4 条（arm1→arm2→Hand、leg1→leg2→leg3 等）；KD-06 识别 4 条且兼容大写变体与中间节点——`Arm1→Arm2→Arm3→Hand`、`Arm1_r→arm_rotation→Arm2_r→Arm3_r→Hand_r`，证明**母版驱动链识别有效**
- FABRIK：可达目标精确收敛（arm1 误差 0.00036，迭代 1）；不可达目标走拉直分支（误差 = 目标距离 − 链总长），行为符合标准 FABRIK
- 已知特性：近伸直奇异构型收敛慢（KD-06 Arm1 链 32 次迭代误差 0.0023），离线验证精度足够，后续可提高迭代上限或加阻尼

**库决策（2026-08-01）**：IK 求解数学为通用算法（FABRIK，Aristidou & Lasenby 2011），开源库可选（Perlin `ik`、论文参考代码、TinyIK 等），但通用库只覆盖"位置求解"部分，链识别与矩阵往返（UKW 特定）仍需自研。**结论：自研 IKSolver 已验证正确，暂不引入第三方库**；若引擎侧需要，接口已按标准 FABRIK 形状设计，替换成本低。

**后续（不在本阶段）**：求解器暂不移入引擎；旋转朝向对齐（部件指向子关节）为可选优化。

