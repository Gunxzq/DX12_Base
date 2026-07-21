# ANI 解析结论 — KD-03

> 日期: 2026-07-09
> 来源: `1.008原版ani/KD-03/`
> 解析工具: AssetTool HODParser（ANI→HOD 转换器）

---

## 概述

原版 KD-03 机体的 ANI 动画文件已成功解析。解析结果输出到 `ANI_Output/` 目录，每组动画包含：

| 文件 | 说明 |
|------|------|
| `Head.txt` | 动画名称（Shift-JIS 编码）+ 帧文件列表 |
| `001~00X_xxx.hod` | 关键帧骨骼姿态（HOD 格式，29 个骨骼节点） |
| `Tail.dat` | 附加数据（240 字节，可能为时间/插值参数） |

---

## 动画分组清单

| 分组 | 名称（解码预估） | 帧数 | 说明 |
|------|-----------------|------|------|
| 01 | 立ち（Stand） | 3 帧 | 待机 |
| 02 | 歩き（Walk） | 9 帧 | 行走（含往返过渡） |
| 03 | ジャンプ開始（JumpStart） | 3 帧 | 跳跃开始 |
| 04 | 飛行停止（FlyStop） | 3 帧 | 飞行停顿 |
| 05 | 着地（Land） | 4 帧 | 着陆 |
| 06 | 着地（Land）-2 | 4 帧 | 着地变体 |
| 07 | 上昇（FlyUp） | 3 帧 | 上升 |
| 08 | 飛行停止（FlyStop）-2 | 4 帧 | 飞行停顿变体 |
| 09 | 左ステップ（LStep） | 3 帧 | 左移步 |
| 10 | 右ステップ（RStep） | 3 帧 | 右移步 |
| 11 | 前ステップ（FStep） | 3 帧 | 前移步 |
| 12 | 後ステップ（BStep） | 3 帧 | 后移步 |
| 13 | 被弾（Hit） | 4 帧 | 受击 |
| 14 | 被弾飛行（HitFly） | 4 帧 | 受击飞行 |
| 15 | 被打上（HitSky） | 4 帧 | 被打飞 |
| 16 | 倒下（Down） | 3 帧 | 倒地 |
| 17 | 起身（Up） | 5 帧 | 站起 |
| 18 | 武器変更（ChangeSword） | 5 帧 | 换武器 |
| 19 | （不明，Shift-JIS 乱码） | 5 帧 | — |
| 20 | 受け身（Ukemi） | 4 帧 | 受身 |
| 21 | ガードダッシュ（GuardDash） | 3 帧 | 防御冲刺 |
| 22 | ブーストダッシュ（BoostDash） | 4 帧 | 加速冲刺 |
| 23 | ガード（Guard） | 3 帧 | 防御 |
| 24 | 待機_S（Stand_S） | 3 帧 | 持剑待机 |
| 25 | 歩き_S（Walk_S） | 9 帧 | 持剑行走 |
| 26 | ジャンプ開始_S（JumpStart_S） | 3 帧 | 持剑跳跃开始 |
| 27 | 飛行停止_S（FlyStop_S） | 3 帧 | 持剑飞行停顿 |
| 28 | 着地_S（Land_S） | 4 帧 | 持剑着陆 |
| 29 | 着地_S（Land_S）-2 | 4 帧 | 持剑着陆变体 |
| 30+ | 后续 | — | 更多持剑/动作变体 |

---

## 骨骼结构（29 节点）

解析结果显示了完整的 KD-03 骨骼层次：

```
root.x
├── Body_d.x
│   └── Body.x
│       ├── Head.x
│       ├── arm1.x → arm2.x → Hand.x
│       │   └── Shield.x → Shield_point.x
│       ├── arm1_r.x → arm2_r.x → Hand_r.x
│       │   ├── gun.x
│       │   ├── sword.x
│       │   └── Weapon_point.x / Weapon_point2.x
│       ├── leg1.x → leg2.x → leg3.x
│       │   ├── Output03.x
│       │   └── Output05.x
│       ├── leg1_r.x → leg2_r.x → leg3_r.x
│       │   └── Output04.x
│       ├── Output01.x
│       ├── Output02.x
│       ├── Output07.x
│       └── Output08.x
```

- **Body_d.x**: 身体变形（变形框架）
- **Body.x**: 上半身核心
- **arm1/arm2/Hand**: 左臂链
- **arm1_r/arm2_r/Hand_r**: 右臂链（持剑/持枪）
- **leg1/leg2/leg3**: 左腿链
- **leg1_r/leg2_r/leg3_r**: 右腿链
- **Output01~08**: 特效/辅助节点（武器发射点等）

---

---

## Tail.dat 结构逆向

`Tail.dat` 是每个动画组的**事件脚本文件**，240 字节，包含：

### 二进制头（16 字节）

| 偏移 | 大小 | 类型 | 值 | 说明 |
|------|------|------|----|------|
| 0x00 | 4 | uint32 | 1 | 版本/标签 |
| 0x04 | 4 | uint32 | 8 | 条目数？ |
| 0x08 | 4 | float | 0.1 | 帧间隔时间（秒）→ 10 FPS |
| 0x0C | 4 | uint32 | 224 | 脚本数据大小 |

### 脚本数据（224 字节）

内嵌 **SPT 格式脚本**，结构为：

```
'ShiftJIS_注释' CRLF
SPT_命令序列 CRLF
ENDIF; CRLF
```

### 解码示例（01 组 - 立ち/Stand）

```
'下半身のみ'       ← 仅下半身动作
IF(@int[151] ,==, 0);
  Move=(0,STOP,0);    ← 停止移动
ENDIF;

'上半身のみ'       ← 仅上半身动作
IF(@int[151] ,==, 1);
  LockArm1Target(-1,-1);
  LockArm2Target(-1,-1);
  LockBodyUpTarget(-1,-1);
  LockBodyDownTarget(-1,-1);
ENDIF;
```

### 关键发现：分层动画系统

`@int[151]` 是控制**上下半身分离动画**的状态标志：

| @int[151] | 含义 | 行为 |
|-----------|------|------|
| 0 | 下半身主导 | 上体自由（用于行走中攻击） |
| 1 | 上半身主导 | 下体锁定（用于站立射击/防御） |

这证实 UKW 引擎采用**分层动画混合**架构，允许上半身（攻击/举枪）与下半身（移动）独立播放不同动画片段。

---

## ANI 脚本命令参考

来源：社区文档 `[ANI] 语句详解.pdf` & `Script.ani剖析与详解.pdf`

### 动画控制

| 命令 | 格式 | 说明 |
|------|------|------|
| `ChangeAnime` | `ChangeAnime(id, flag, ?)` | 切换到指定动画 ID |
| `ChangeWeapon` | `ChangeWeapon(WEAPON_TYPE)` | 切换武器（GUN/SWORD等） |
| `AnimeLoop` | `AnimeLoop = 0/1` | 动画循环标志 |
| `GoPoseIndex` | `GoPoseIndex(-1,2)` | 跳转到指定姿势索引 |
| `GoScriptIndex` | `GoScriptIndex(7)` | 跳转到指定脚本索引 |
| `ExecScriptEveryTime` | `ExecScriptEveryTime(0/1)` | 每帧执行脚本 |

### 骨骼/IK 锁定

| 命令 | 格式 | 说明 |
|------|------|------|
| `LockArm1Target` | `LockArm1Target(strength, duration)` | 锁定左臂 IK 目标 |
| `LockArm2Target` | `LockArm2Target(strength, duration)` | 锁定右臂 IK 目标 |
| `LockBodyUpTarget` | `LockBodyUpTarget(strength, duration)` | 锁定上半身 IK |
| `LockBodyDownTarget` | `LockBodyDownTarget(strength, duration)` | 锁定下半身 IK |

参数 `(-1,-1)` 表示解除锁定。

### 移动控制

| 命令 | 格式 | 说明 |
|------|------|------|
| `Move` | `Move=(x, mode, speed)` | 设置移动。mode: STOP=0, 方向等 |
| `MoveLock` | `MoveLock()` | 锁定移动位置 |
| `Force` | `Force=(x, y, z)` | 施加外力/速度 |
| `BoostDashMode` | `BoostDashMode = 0/1` | 冲刺模式 |

### 攻击判定

| 命令 | 格式 | 说明 |
|------|------|------|
| `ATTACK` | `ATTACK(dmg, ?, ?, ?)` | 攻击判定框定义 |
| `AttackDelay` | `AttackDelay(frames)` | 攻击判定延迟 |
| `AttackDown` | `AttackDown = value` | 击倒积蓄值 |
| `AttackFlag` | `AttackFlag = bits` | 攻击属性标志 |
| `AttackForce` | `AttackForce = value` | 攻击击退力 |
| `AttackPow` | `AttackPow = value` | 攻击威力系数 |

### 特效/状态

| 命令 | 格式 | 说明 |
|------|------|------|
| `BURNER` | `BURNER(index)` | 推进器喷焰效果（0-5索引） |
| `CamEffect` | `CamEffect = mode` | 镜头效果 |
| `AddEnergy` | `AddEnergy = value` | 增加能量（HP） |
| `AddExGauge` | `AddExGauge = value` | 增加EX槽 |
| `RunProc2` | `RunProc2(...)` | 运行子进程（特效/弹幕） |
| `LaserReflect` | `LaserReflect = value` | 激光反射属性 |
| `LocalRnd` | `LocalRnd(min, max, ?)` | 本地随机数 |
| `CatchLastChara` | `CatchLastChara(?, ?, ?)` | 捕捉最后命中的角色（连段用） |

---

## @int / @float 游戏变量参考

来源：社区文档 `[ANI] int&float表格&详解.pdf`

### 核心变量

| 变量 | 说明 |
|------|------|
| `@int[100]` | 能量/HP |
| `@int[101]` | Script.spt 场景编号 |
| `@int[102]` | [101] 的 X 坐标 |
| `@int[103]` | EX 槽（最大 2000） |
| `@float[100]` | 能量浮点（用于 HUD） |

### 状态变量

| 变量 | 值 | 说明 |
|------|----|------|
| `@int[150]` | 0=地上 1=空中 | 在地面/空中状态 |
| **`@int[151]`** | **0=下半身 1=上半身** | **上下半身分离动画（核心状态）** |
| `@int[152]` | 0=地上 1=空中 | 状态变体 |
| `@int[153]` | 0=未命中 | 攻击命中检测 |
| `@int[154]` | — | [106] Y 坐标 |
| `@int[155]` | EX等级(0-5) | EX 槽等级（[107]Z, CamEffect=4） |
| `@int[156]` | — | [108] float 值 |
| `@int[157]-159` | — | 坐标/姿态 |
| `@int[160]` | 0=关 1=开 | OC（OverCharge）状态 |
| `@int[161-180]` | — | AI 专用变量 |
| `@int[181]` | 0=通常 1=Step 2=Jump ... | 移动状态机 |
| `@int[190]` | 方向值 | 方向键输入 |
| `@int[191-199]` | — | 扩展状态 |
| `@int[0-98]` | 用户定义 | 用户自定义变量（HOD/Tail 中使用） |
| `@float[0-98]` | 用户定义 | 用户自定义浮点变量 |

---

## 社区工具说明

来源：`ウィンダムXP 游戏工具包 v1.4`

### ANI 解析流程

社区工具 `[ANI].exe` 的解析流程：
1. 读取 `Script.ani` 中的 ANI 条目
2. 根据条目名称从 `HODFile/` 读取对应文件
3. 输出到 `ANI_Output/{条目名}/` 目录
4. 生成 `Head.txt`（动画名称 + 帧文件列表）
5. 生成 N 个 `.hod` 文件（每帧骨骼姿态）
6. 生成 `Tail.dat`（240 字节状态机脚本）

### ANI_list.txt

`ANI_list.txt` 记录所有 ANI 条目的索引：

```
ANIRobo.hod   ← 原始 ANI 文件
Note: KD-03   ← 机体名
Amount: 3 Files  ← 动画组数量
                  ← 空行后的缩进表示动画条目
 01\001_robo_stand.hod  1
 01\002_robo_stand.hod  2
 01\003_robo_stand.hod  3
 Tail: 240 Bytes
```

### 格式限制

社区工具有以下 limitations：
- 无法批量处理，每次只能转换一个 ANI 条目
- 依赖 `comctl32.ocx` ActiveX 控件（老旧 Windows 组件）
- UI 为日文，仅支持单文件操作
- 输出路径固定，不支持自定义

---

## 关键技术结论

### 1. ANI → HOD 格式兼容
- ANI 解析后输出为 HOD 格式，每个 `.hod` 文件对应一帧的完整骨骼姿态
- 29 个骨骼节点的变换矩阵（4x4 float）均正确解析
- A/B 前移机制已验证通过

### 2. Head.txt 编码
- Shift-JIS 编码，`立ち` 解码显示为 `棫偪`（需正确转码为 UTF-8）
- 文件名为日文，转换工具中需注意编码处理

### 3. Tail.dat（240 字节）
- 疑为帧间插值参数或时间控制数据
- 具体格式待进一步逆向分析

### 4. 蒙皮数据关联
- 骨骼名称与 `KD-03.x` 模型文件中的骨骼命名一致（`Body.x`, `Head.x`, `arm1.x` 等）
- 蒙皮顶点使用 `BLENDINDICES`（R8G8B8A8_UINT）和 `BONEWEIGHTS`（float4）
- 每个顶点的骨骼索引指向此骨骼树中的节点

---

## 后续工作

- [ ] Tail.dat 格式逆向
- [ ] 帧间插值算法实现（线性/球面插值四元数）
- [ ] ANI 解析器集成到 AssetTool CLI
- [ ] 动画剪辑导出格式定义（AnimationClip → .anim 格式）
- [ ] 引擎动画 System 接入（SkeletalAnimationSystem）
